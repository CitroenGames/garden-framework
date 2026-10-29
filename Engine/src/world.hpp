#pragma once

#include "Components/Components.hpp"
#include "Components/camera.hpp"
#include "EngineExport.h"
#include "ECS/ComponentStore.hpp"
#include "ECS/EntityHandle.hpp"
#include "ECS/ComponentOperations.hpp"
#include "PhysicsSystem.hpp"
#include "Tick/TickSystem.hpp"
#include <cstdint>
#include <span>
#include <array>
#include <functional>
#include <type_traits>
#include <unordered_map>
#include <vector>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <entt/entt.hpp>

class LevelManager;
class world;
struct RetirementFence;

enum class LifecycleStatus {
    Completed, CompletedWithObserverFailure, Queued, AlreadyPending, Alive, Pending,
    Invalid, WrongWorld, WrongThread, Stale, QueryActive, Reentrant, AllocationFailure,
    ObserverFailure, Partial, InternalFailure, Conflict, UnknownOperation
};
enum class LifecyclePhase { None, Prepare, Constraints, Event, Hierarchy, Physics, Components, Identity };
enum class LifecycleOperationKind { None, PublishedBatch, UnpublishedRollback };
class LifecycleOperationHandle {
public:
    LifecycleOperationHandle() noexcept = default;
    bool valid() const noexcept { return owner_ && incarnation_ && serial_; }
    friend bool operator==(const LifecycleOperationHandle&, const LifecycleOperationHandle&) noexcept = default;
private:
    friend class world;
    const world* owner_ = nullptr;
    uint64_t incarnation_ = 0;
    uint64_t serial_ = 0;
};
// `completed` counts entities retired by this operation, including those
// completed before an error. `entity` and `phase` identify interrupted work.
struct LifecycleResult {
    LifecycleStatus status = LifecycleStatus::Completed;
    LifecyclePhase phase = LifecyclePhase::None;
    garden::ecs::EntityHandle entity{};
    size_t completed = 0;
    LifecycleOperationHandle operation{};
    LifecycleOperationKind kind = LifecycleOperationKind::None;
    LifecycleOperationHandle blocked_by{};
    LifecycleOperationKind blocked_by_kind = LifecycleOperationKind::None;
    bool accepted = false;
    bool cleanupComplete() const noexcept {
        return status == LifecycleStatus::Completed || status == LifecycleStatus::CompletedWithObserverFailure;
    }
};

// Internal fault-injection context for lifecycle fixtures. Installed only by tests.
namespace garden::ecs::testing {
struct LifecycleFaultHook {
    void* context = nullptr;
    bool (*fail)(void*, LifecyclePhase) noexcept = nullptr;
};
struct LifecycleTestAccess;
}

class ConstructionHandle {
public:
    bool valid() const noexcept { return owner_ && incarnation_ && serial_; }
    friend bool operator==(const ConstructionHandle&, const ConstructionHandle&) noexcept = default;
private:
    friend class world;
    const world* owner_ = nullptr;
    uint64_t incarnation_ = 0, serial_ = 0;
};
class StagedEntity {
public:
    bool valid() const noexcept { return transaction_.valid() && member_serial_; }
    friend bool operator==(const StagedEntity&, const StagedEntity&) noexcept = default;
private:
    friend class world;
    ConstructionHandle transaction_{};
    uint64_t member_serial_ = 0;
};
enum class ConstructionStatus { Completed, InvalidInput, StaleTransaction, WrongWorld, Conflict, WrongThread, QueryActive, Reentrant, AllocationFailure, ComponentFailure, EventQueueFailure, TransactionFailed };
struct ConstructionBeginResult { ConstructionStatus status; ConstructionHandle transaction{}; };
struct StagedEntityResult { ConstructionStatus status; StagedEntity entity{}; EntityRef reference{}; garden::ecs::EntityHandle captured{}; };
struct ConstructionCommitResult { ConstructionStatus status; std::vector<garden::ecs::EntityHandle> entities; std::span<const garden::ecs::EntityHandle> captured; };
enum class StagedLookupStatus { Found, NotFound, InvalidInput, StaleTransaction, WrongWorld, WrongThread, QueryActive, Reentrant, TransactionFailed, Conflict };
struct StagedLookupResult { StagedLookupStatus status; StagedEntity entity{}; };
struct ConstructionAbortResult { ConstructionStatus status; LifecycleResult rollback{}; std::span<const garden::ecs::EntityHandle> captured; };
struct ConstructionComponentResult { ConstructionStatus transaction_status; garden::ecs::ComponentStoreResult component_status; };

enum class ScopedRestoreStatus { Completed, InvalidInput, Conflict, QueryActive, AllocationFailure, ConstructionFailure };
struct ScopedRestoreResult {
    ScopedRestoreStatus status = ScopedRestoreStatus::Completed;
    std::vector<garden::ecs::EntityHandle> entities;
    LifecycleResult rollback{};
};
struct EntityCreationResult {
    ScopedRestoreStatus status = ScopedRestoreStatus::Completed;
    EntityRef reference{};
    garden::ecs::EntityHandle entity{};
    LifecycleResult rollback{};
};
enum class EntityHandleClassification { Alive, Pending, Retiring, RetiredGone, WrongWorld, Unverifiable };

namespace GameFramework
{
    class GameFrameworkRuntime;
    class GameModeBase;
    class GameStateBase;
}

class ENGINE_API world
{
private:
    // CharacterPhysicsService implements its Garden handle API through this private seam.
    friend class CharacterPhysicsService;
    friend class LevelManager;
    friend struct garden::ecs::testing::LifecycleTestAccess;
    struct ConstructionState;
    std::unique_ptr<ConstructionState> construction_state;
    ConstructionStatus validateConstruction(ConstructionHandle, bool allow_failed = false) const noexcept;
    ConstructionStatus stagedKey(ConstructionHandle, StagedEntity, garden::ecs::ComponentEntityKey&) const noexcept;
    entt::entity constructionEntity(ConstructionHandle, StagedEntity) const noexcept;
    void failConstruction(ConstructionHandle) noexcept;
    void constructionTransition(bool) noexcept;
    bool constructionReentrant() const noexcept;
    bool constructionActive() const noexcept;
    class ConstructionAuthoringScope {
    public:
        ConstructionAuthoringScope(world&, ConstructionHandle) noexcept;
        ~ConstructionAuthoringScope();
        ConstructionStatus status() const noexcept { return status_; }
        ConstructionAuthoringScope(const ConstructionAuthoringScope&) = delete;
        ConstructionAuthoringScope& operator=(const ConstructionAuthoringScope&) = delete;
    private:
        world* owner_ = nullptr;
        ConstructionStatus status_ = ConstructionStatus::InvalidInput;
    };
    bool isStaged(entt::entity) const noexcept;
    bool stagedInvariant(ConstructionHandle, StagedEntity) const noexcept;
    StagedEntityResult stageEntity(ConstructionHandle, EntityRef, std::string_view, bool);
    struct LifecycleState;
    std::unique_ptr<LifecycleState> lifecycle_state;
    uint64_t incarnation = 0;
    uint64_t next_operation_serial = 1;
    std::unique_ptr<RetirementFence> retirement_fence;
    LifecycleResult operationConflict() const noexcept;
    LifecycleResult operationProgress(LifecycleResult) noexcept;
    LifecycleStatus validateLifecycleHandle(garden::ecs::EntityHandle) const noexcept;
    LifecycleResult retireRaw(entt::entity, bool unpublished) noexcept;
    LifecycleResult retireEntitiesImpl(std::span<const garden::ecs::EntityHandle>, bool external) noexcept;
    LifecycleResult processRetirement() noexcept;
    void markWorldActive() noexcept;
    struct ComponentWorldState;
    std::unique_ptr<ComponentWorldState> component_state;
    std::unique_ptr<PhysicsSystem> physics_system;
    std::unique_ptr<GameFramework::GameFrameworkRuntime> gameplay_framework;
    Tick::FixedTickAccumulator simulation_ticks;
    uint32_t simulation_tick = 0;
    double simulation_time = 0.0;
    uint32_t active_queries = 0;

    garden::ecs::ComponentStore& componentStore() noexcept;
    const garden::ecs::ComponentStore& componentStore() const noexcept;
    garden::ecs::ComponentStoreResult scopedKey(garden::ecs::EntityHandle, garden::ecs::ComponentEntityKey&) const noexcept;
    using IdentityCounts = std::unordered_map<EntityGuid, size_t, GuidHash>;
    const IdentityCounts* query_identity_counts = nullptr;
    garden::ecs::ComponentStoreResult snapshotIdentityCounts(IdentityCounts&) const noexcept;
    garden::ecs::EntityHandle handleForKey(garden::ecs::ComponentEntityKey, const IdentityCounts&) const noexcept;
    static garden::ecs::ComponentStoreResult validateBackend(garden::ecs::ComponentKey, garden::ecs::ComponentId) noexcept;
    bool isAdapted(garden::ecs::ComponentId) const noexcept;
    garden::ecs::ComponentStoreResult addAdapted(garden::ecs::ComponentId, garden::ecs::ComponentEntityKey, bool (*)(void*, void*) noexcept, void*) noexcept;
    garden::ecs::ComponentStoreResult getValue(garden::ecs::ComponentId, garden::ecs::ComponentEntityKey, void*&) const noexcept;
    garden::ecs::ComponentStoreResult removeValue(garden::ecs::ComponentId, garden::ecs::ComponentEntityKey) noexcept;
    garden::ecs::ComponentStoreResult snapshotValues(garden::ecs::ComponentId, std::vector<garden::ecs::ComponentEntityKey>&) const noexcept;

    template<class T>
    static garden::ecs::ComponentStoreResult resolveType(garden::ecs::ComponentId& id) noexcept
    {
        using namespace garden::ecs;
        constexpr uint32_t capabilities = [] {
            if constexpr (requires { ComponentType<T>::capabilities; })
                return static_cast<uint32_t>(ComponentType<T>::capabilities);
            else return componentCapabilities<T>();
        }();
        const auto result = ComponentCatalog::instance().resolveType(ComponentType<T>::key,
            sizeof(T), alignof(T), capabilities, id);
        switch (result) {
        case ComponentCatalogResult::Success: return validateBackend(ComponentType<T>::key, id);
        case ComponentCatalogResult::NotFound: return ComponentStoreResult::UnregisteredType;
        case ComponentCatalogResult::Conflict: return ComponentStoreResult::TypeMismatch;
        case ComponentCatalogResult::ResourceExhausted: return ComponentStoreResult::ResourceExhausted;
        case ComponentCatalogResult::InternalError: return ComponentStoreResult::InternalError;
        default: return ComponentStoreResult::StaleComponent;
        }
    }

    // Private bridge from Garden identity to the ECS runtime.
    entt::entity resolveEntity(EntityRef entity) const;

    void clearRegistryStorage()
    {
        registry.clear();

        std::vector<entt::id_type> storage_ids;
        for (auto&& [id, storage] : registry.storage())
            storage_ids.push_back(id);

        for (entt::id_type id : storage_ids)
            registry.reset(id);
    }

public:
    entt::registry registry;
    camera world_camera;
    float fixed_delta;

    explicit world(const PhysicsSystemSettings& physics_settings = PhysicsSystemSettings());

    world(world&&) = delete;
    world& operator=(world&&) = delete;
    world(const world&) = delete;
    world& operator=(const world&) = delete;

    ~world();

    ConstructionBeginResult beginConstruction() noexcept;
    StagedEntityResult createStagedEntity(ConstructionHandle, std::string_view tag = {});
    StagedEntityResult restoreStagedEntity(ConstructionHandle, EntityRef, std::string_view tag = {});
    StagedLookupResult resolveStagedReference(ConstructionHandle, EntityRef) const noexcept;
    ConstructionCommitResult commitConstruction(ConstructionHandle);
    ConstructionAbortResult abortConstruction(ConstructionHandle) noexcept;
    ConstructionStatus constructionStatus(ConstructionHandle) const noexcept;

    template<class T, class... Args>
    ConstructionComponentResult add(ConstructionHandle tx, StagedEntity entity, Args&&... args) {
        using namespace garden::ecs;
        ComponentEntityKey key{}; auto status = stagedKey(tx, entity, key);
        if (status != ConstructionStatus::Completed) return {status, ComponentStoreResult::InvalidEntity};
        ComponentId id{}; auto result = resolveType<T>(id);
        if (result != ComponentStoreResult::Success) return {ConstructionStatus::Completed, result};
        constructionTransition(true);
        if (isAdapted(id)) {
            auto arguments = std::forward_as_tuple(std::forward<Args>(args)...);
            auto construct = [](void* destination, void* context) noexcept -> bool {
                try { auto& tuple = *static_cast<decltype(arguments)*>(context);
                    std::apply([destination](auto&&... v) { ::new (destination) T(std::forward<decltype(v)>(v)...); }, std::move(tuple)); return true;
                } catch (...) { return false; }
            };
            result = addAdapted(id, key, construct, &arguments);
        } else result = componentStore().add<T>(id, key, std::forward<Args>(args)...);
        constructionTransition(false);
        if (result != ComponentStoreResult::Success && result != ComponentStoreResult::Duplicate) failConstruction(tx);
        if (!stagedInvariant(tx, entity)) { failConstruction(tx); return {ConstructionStatus::TransactionFailed, result}; }
        return {constructionStatus(tx), result};
    }
    template<class T> ConstructionComponentResult get(ConstructionHandle tx, StagedEntity entity, T*& out) {
        out = nullptr;
        using namespace garden::ecs; ComponentEntityKey key{}; auto status = stagedKey(tx, entity, key);
        if (status != ConstructionStatus::Completed) return {status, ComponentStoreResult::InvalidEntity};
        ComponentId id{}; auto result = resolveType<T>(id); if (result != ComponentStoreResult::Success) return {status, result};
        void* value = nullptr; result = getValue(id, key, value); if (result == ComponentStoreResult::Success) out = static_cast<T*>(value);
        return {status, result};
    }
    template<class T> ConstructionComponentResult has(ConstructionHandle tx, StagedEntity entity, bool& out) const {
        out = false;
        using namespace garden::ecs; ComponentEntityKey key{}; auto status = stagedKey(tx, entity, key);
        if (status != ConstructionStatus::Completed) return {status, ComponentStoreResult::InvalidEntity};
        ComponentId id{}; auto result = resolveType<T>(id); if (result != ComponentStoreResult::Success) return {status, result};
        void* value = nullptr; result = getValue(id, key, value);
        if (result == ComponentStoreResult::NotFound) { out = false; result = ComponentStoreResult::Success; }
        else if (result == ComponentStoreResult::Success) out = true;
        return {status, result};
    }
    template<class T> ConstructionComponentResult remove(ConstructionHandle tx, StagedEntity entity) {
        using namespace garden::ecs; ComponentEntityKey key{}; auto status = stagedKey(tx, entity, key);
        if (status != ConstructionStatus::Completed) return {status, ComponentStoreResult::InvalidEntity};
        ComponentId id{}; auto result = resolveType<T>(id); if (result != ComponentStoreResult::Success) return {status, result};
        constructionTransition(true); result = removeValue(id, key); constructionTransition(false);
        if (result != ComponentStoreResult::Success && result != ComponentStoreResult::NotFound) failConstruction(tx);
        if (!stagedInvariant(tx, entity)) { failConstruction(tx); return {ConstructionStatus::TransactionFailed, result}; }
        return {constructionStatus(tx), result};
    }

    // Creates a Garden entity handle with a stable identity. Gameplay code uses
    // this handle instead of storage-library entity values.
    EntityCreationResult createEntity(std::string_view tag = {});
    EntityCreationResult createScopedEntity(std::string_view tag = {});
    ScopedRestoreResult restoreScopedEntity(EntityRef reference);
    ScopedRestoreResult restoreScopedEntities(std::span<const EntityRef> references);
    EntityHandleClassification classifyEntityHandle(garden::ecs::EntityHandle) const noexcept;
    garden::ecs::EntityHandle scopedEntity(EntityRef entity);
    // Returns null for invalid handles or missing, null, or ambiguous identity.
    // This read does not assign identity or scope and is safe during queries.
    EntityRef persistedReference(garden::ecs::EntityHandle entity) const;
    bool isEntityValid(garden::ecs::EntityHandle entity) const noexcept;
    // Requests are FIFO and may be made inside a query. Pending entities are
    // omitted from subsequent query callbacks; the current callback completes.
    // A failed observer attempt is never replayed on retry.
    LifecycleResult requestRetirement(garden::ecs::EntityHandle entity) noexcept;
    LifecycleResult retireEntity(garden::ecs::EntityHandle entity) noexcept;
    LifecycleResult retireEntities(std::span<const garden::ecs::EntityHandle>) noexcept;
    LifecycleResult resumeRetirement(LifecycleOperationHandle) noexcept;
    LifecycleResult retirementResult(LifecycleOperationHandle) const noexcept;
    LifecycleResult acknowledgeRetirement(LifecycleOperationHandle) noexcept;
    LifecycleResult drainRetirements() noexcept;
    LifecycleResult retirementState(garden::ecs::EntityHandle entity) const noexcept;

    template<class T, class... Args>
    garden::ecs::ComponentStoreResult add(garden::ecs::EntityHandle entity, Args&&... args)
    {
        if (active_queries) return garden::ecs::ComponentStoreResult::StructuralMutationDuringQuery;
        if (constructionReentrant() || constructionActive()) return garden::ecs::ComponentStoreResult::InternalError;
        garden::ecs::ComponentEntityKey key;
        auto result = scopedKey(entity, key);
        if (result != garden::ecs::ComponentStoreResult::Success) return result;
        garden::ecs::ComponentId id;
        result = resolveType<T>(id);
        if (result != garden::ecs::ComponentStoreResult::Success) return result;
        if (isAdapted(id)) {
            auto arguments = std::forward_as_tuple(std::forward<Args>(args)...);
            auto construct = [](void* destination, void* context) noexcept -> bool {
                try {
                    auto& tuple = *static_cast<decltype(arguments)*>(context);
                    std::apply([destination](auto&&... values) {
                        ::new (destination) T(std::forward<decltype(values)>(values)...);
                    }, std::move(tuple));
                    return true;
                } catch (...) { return false; }
            };
            return addAdapted(id, key, construct, &arguments);
        }
        return componentStore().add<T>(id, key, std::forward<Args>(args)...);
    }

    // Pointers can be invalidated by structural changes to the same component
    // pool, entity destruction, or world reset/destruction. Synchronize access.
    template<class T>
    garden::ecs::ComponentStoreResult get(garden::ecs::EntityHandle entity, T*& out)
    {
        out = nullptr;
        garden::ecs::ComponentEntityKey key;
        auto result = scopedKey(entity, key);
        if (result != garden::ecs::ComponentStoreResult::Success) return result;
        garden::ecs::ComponentId id;
        result = resolveType<T>(id);
        if (result != garden::ecs::ComponentStoreResult::Success) return result;
        void* value = nullptr;
        result = getValue(id, key, value);
        if (result == garden::ecs::ComponentStoreResult::Success) out = static_cast<T*>(value);
        return result;
    }

    template<class T>
    garden::ecs::ComponentStoreResult get(garden::ecs::EntityHandle entity, const T*& out) const
    {
        out = nullptr;
        garden::ecs::ComponentEntityKey key;
        auto result = scopedKey(entity, key);
        if (result != garden::ecs::ComponentStoreResult::Success) return result;
        garden::ecs::ComponentId id;
        result = resolveType<T>(id);
        if (result != garden::ecs::ComponentStoreResult::Success) return result;
        void* value = nullptr;
        result = getValue(id, key, value);
        if (result == garden::ecs::ComponentStoreResult::Success) out = static_cast<const T*>(value);
        return result;
    }

    template<class T>
    garden::ecs::ComponentStoreResult has(garden::ecs::EntityHandle entity, bool& out) const
    {
        out = false;
        garden::ecs::ComponentEntityKey key;
        auto result = scopedKey(entity, key);
        if (result != garden::ecs::ComponentStoreResult::Success) return result;
        garden::ecs::ComponentId id;
        result = resolveType<T>(id);
        if (result != garden::ecs::ComponentStoreResult::Success) return result;
        void* value = nullptr;
        result = getValue(id, key, value);
        if (result == garden::ecs::ComponentStoreResult::NotFound)
            return garden::ecs::ComponentStoreResult::Success;
        if (result == garden::ecs::ComponentStoreResult::Success)
            out = true;
        return result;
    }

    template<class T>
    garden::ecs::ComponentStoreResult remove(garden::ecs::EntityHandle entity)
    {
        if (active_queries) return garden::ecs::ComponentStoreResult::StructuralMutationDuringQuery;
        if (constructionReentrant() || constructionActive()) return garden::ecs::ComponentStoreResult::InternalError;
        garden::ecs::ComponentEntityKey key;
        auto result = scopedKey(entity, key);
        if (result != garden::ecs::ComponentStoreResult::Success) return result;
        garden::ecs::ComponentId id;
        result = resolveType<T>(id);
        if (result != garden::ecs::ComponentStoreResult::Success) return result;
        return removeValue(id, key);
    }

    // Order is unspecified. The callback receives a scoped handle and references
    // to all requested components; explicit const types yield const references.
    // Queries may nest, but Garden structural mutations are rejected until all
    // callbacks return. World shutdown/reset report QueryActive during a query. Direct
    // registry mutations bypass this guard and must not occur during a query.
    // Identity uniqueness is checked against a read-only snapshot at query entry.
    // Access is externally synchronized;
    // the world must outlive every callback.
    template<class... Ts, class Callback>
    garden::ecs::ComponentStoreResult query(Callback&& callback)
    {
        using namespace garden::ecs;
        static_assert(sizeof...(Ts) > 0, "A query requires at least one component type");
        static_assert(((!std::is_reference_v<Ts> && !std::is_volatile_v<Ts>) && ...));
        if constexpr (sizeof...(Ts) == 0) return ComponentStoreResult::InternalError;
        else {
            struct Guard {
                world& owner;
                explicit Guard(world& w) : owner(w) { ++owner.active_queries; }
                ~Guard() { --owner.active_queries; }
            } guard(*this);
            std::array<ComponentId, sizeof...(Ts)> ids{};
            ComponentStoreResult result = ComponentStoreResult::Success;
            size_t index = 0;
            ((result == ComponentStoreResult::Success
                ? result = resolveType<std::remove_const_t<Ts>>(ids[index++]) : result), ...);
            if (result != ComponentStoreResult::Success) return result;
            if (!component_state) return ComponentStoreResult::Success;
            std::vector<ComponentEntityKey> candidates;
            bool first = true;
            for (const auto id : ids) {
                std::vector<ComponentEntityKey> pool;
                result = snapshotValues(id, pool);
                if (result != ComponentStoreResult::Success) return result;
                if (first || pool.size() < candidates.size()) candidates.swap(pool);
                first = false;
            }
            IdentityCounts identityCounts;
            result = snapshotIdentityCounts(identityCounts);
            if (result != ComponentStoreResult::Success) return result;
            struct IdentitySnapshotGuard {
                world& owner;
                const IdentityCounts* previous;
                IdentitySnapshotGuard(world& w, const IdentityCounts& counts)
                    : owner(w), previous(w.query_identity_counts) { owner.query_identity_counts = &counts; }
                ~IdentitySnapshotGuard() { owner.query_identity_counts = previous; }
            } identityGuard(*this, identityCounts);
            for (const auto key : candidates) {
                std::array<void*, sizeof...(Ts)> values{};
                bool match = true;
                for (size_t i = 0; i < ids.size(); ++i) {
                    result = getValue(ids[i], key, values[i]);
                    if (result == ComponentStoreResult::NotFound) { match = false; break; }
                    if (result != ComponentStoreResult::Success) return result;
                }
                if (!match) continue;
                const auto handle = handleForKey(key, identityCounts);
                if (!handle.m_world || retirementState(handle).status == LifecycleStatus::Pending) continue;
                try {
                    [&]<size_t... I>(std::index_sequence<I...>) {
                        std::invoke(callback, handle, *static_cast<Ts*>(values[I])...);
                    }(std::index_sequence_for<Ts...>{});
                } catch (...) { return ComponentStoreResult::CallbackFailed; }
            }
            return ComponentStoreResult::Success;
        }
    }

    // Returned component pointers may be invalidated by structural changes to
    // that component type's storage, including adding/removing it on another
    // entity, or by destroying the owning entity. Invalid handles and absent
    // components return null. Callers must not retain pointers across such
    // changes.
    bool isEntityValid(EntityRef entity) const;

    template<typename T>
    bool hasComponent(EntityRef entity) const
    {
        const auto resolved = resolveEntity(entity);
        return resolved != entt::null && registry.all_of<T>(resolved);
    }

    template<typename T>
    T* tryGetComponent(EntityRef entity)
    {
        const auto resolved = resolveEntity(entity);
        return resolved != entt::null ? registry.try_get<T>(resolved) : nullptr;
    }

    template<typename T>
    const T* tryGetComponent(EntityRef entity) const
    {
        const auto resolved = resolveEntity(entity);
        return resolved != entt::null ? registry.try_get<T>(resolved) : nullptr;
    }

    // Returns the uniquely named entity's existing identity, or a null handle
    // if the name is empty, absent, ambiguous, or has no non-null identity.
    EntityRef findEntityByTag(const std::string& name) const;

    void setAuthorityGameMode(std::unique_ptr<GameFramework::GameModeBase> game_mode);
    GameFramework::GameModeBase* getAuthorityGameMode();
    const GameFramework::GameModeBase* getAuthorityGameMode() const;

    template<typename T>
    T* getAuthorityGameModeAs()
    {
        return dynamic_cast<T*>(getAuthorityGameMode());
    }

    template<typename T>
    const T* getAuthorityGameModeAs() const
    {
        return dynamic_cast<const T*>(getAuthorityGameMode());
    }

    void setGameState(std::unique_ptr<GameFramework::GameStateBase> state);
    GameFramework::GameStateBase* getGameState();
    const GameFramework::GameStateBase* getGameState() const;

    template<typename T>
    T* getGameStateAs()
    {
        return dynamic_cast<T*>(getGameState());
    }

    template<typename T>
    const T* getGameStateAs() const
    {
        return dynamic_cast<const T*>(getGameState());
    }

    void clearGameplayFramework();
    bool initializeGameplayFramework(const std::string& map_name = "", const std::string& options = "");
    void startGameplayFramework();
    void tickGameplayFramework(float delta_time);
    void shutdownGameplayFramework();
    bool isGameplayFrameworkInitialized() const;
    bool hasGameplayFrameworkStarted() const;

    // Initialize Jolt physics (call after world construction)
    void initializePhysics()
    {
        markWorldActive();
        physics_system->initialize();
    }

    // Physics system access
    PhysicsSystem& getPhysicsSystem() { return *physics_system; }
    const PhysicsSystem& getPhysicsSystem() const { return *physics_system; }

    // Simplified interface that delegates to physics system
    uint32_t step_physics(float dt)
    {
        simulation_ticks.setFixedDelta(fixed_delta);
        const uint32_t steps = simulation_ticks.consume(dt);

        for (uint32_t i = 0; i < steps; ++i)
        {
            physics_system->stepPhysics(registry);
            ++simulation_tick;
            simulation_time += fixed_delta;
        }

        return steps;
    }

    // One fixed physics step without accumulator consumption.
    // Used by SystemScheduler's Physics phase, which owns accumulation.
    void stepPhysicsSingleStep()
    {
        physics_system->stepPhysics(registry);
        ++simulation_tick;
        simulation_time += fixed_delta;
    }

    uint32_t getSimulationTick() const { return simulation_tick; }
    double getWorldTime() const { return simulation_time; }
    float getFixedDelta() const { return fixed_delta; }
    Tick::SimulationTickContext getTickContext(uint32_t substep_index, uint32_t substep_count) const
    {
        Tick::SimulationTickContext context;
        context.tick_index = simulation_tick + substep_index;
        context.substep_index = substep_index;
        context.substep_count = substep_count;
        context.fixed_delta = fixed_delta;
        context.world_time = simulation_time + static_cast<double>(substep_index) * fixed_delta;
        return context;
    }

    float getPhysicsInterpolationAlpha() const { return simulation_ticks.getAlpha(); }

    // D6 trigger polling (also invoked automatically at end of step_physics;
    // calling again is a no-change diff, so scheduler systems stay exactly-once).
    void update_triggers()
    {
        physics_system->updateTriggers(registry);
    }

    PhysicsSystem::RaycastResult raycastClosest(const glm::vec3& origin, const glm::vec3& direction,
        float max_distance, const PhysicsSystem::QueryFilter& filter)
    {
        return physics_system->raycastClosest(origin, direction, max_distance, filter);
    }

    uint32_t overlapSphere(const glm::vec3& center, float radius,
        const PhysicsSystem::QueryFilter& filter, PhysicsSystem::OverlapResult* out, uint32_t cap)
    {
        return physics_system->overlapSphere(center, radius, filter, out, cap);
    }

    // Utility methods for common physics queries
    bool raycast(const glm::vec3& origin, const glm::vec3& direction, float max_distance,
        glm::vec3& hit_point, glm::vec3& hit_normal)
    {
        return physics_system->raycast(origin, direction, max_distance, registry, hit_point, hit_normal);
    }

    PhysicsSystem::RaycastResult raycastClosest(const glm::vec3& origin, const glm::vec3& direction,
        float max_distance, entt::entity ignored_entity = entt::null)
    {
        return physics_system->raycastClosest(origin, direction, max_distance, ignored_entity);
    }

    // Shape casting convenience wrappers
    PhysicsSystem::ShapeCastResult sphereCast(const glm::vec3& origin, float radius,
        const glm::vec3& direction, float maxDistance)
    {
        return physics_system->sphereCast(origin, radius, direction, maxDistance);
    }

    PhysicsSystem::ShapeCastResult boxCast(const glm::vec3& origin, const glm::vec3& halfExtents,
        const glm::vec3& rotation, const glm::vec3& direction, float maxDistance)
    {
        return physics_system->boxCast(origin, halfExtents, rotation, direction, maxDistance);
    }

    bool teleport_body(entt::entity entity, const glm::vec3& position, const glm::vec3& euler_degrees)
    {
        return physics_system->teleportBody(registry, entity, position, euler_degrees);
    }

    // Configuration methods
    void setGravity(const glm::vec3& gravity)
    {
        physics_system->setGravity(gravity);
    }

    glm::vec3 getGravity() const
    {
        return physics_system->getGravity();
    }

    void setFixedDelta(float delta)
    {
        physics_system->setFixedDelta(delta);
        fixed_delta = physics_system->getFixedDelta();
        simulation_ticks.setFixedDelta(fixed_delta);
    }

    bool configurePhysics(const PhysicsSystemSettings& physics_settings)
    {
        if (!physics_system->configure(physics_settings))
            return false;
        fixed_delta = physics_system->getFixedDelta();
        simulation_ticks.setFixedDelta(fixed_delta);
        simulation_ticks.reset();
        simulation_tick = 0;
        simulation_time = 0.0;
        return true;
    }

    // A partial result retains retirement progress for a later retry.
    LifecycleResult shutdown() noexcept;

    // Full reset: tear down physics, clear ECS, reinitialize.
    // Used by the editor to restore pre-play state.
    LifecycleResult resetWorld(const PhysicsSystemSettings& settings = PhysicsSystemSettings{}) noexcept;
};

// Test access is intentionally separate from world operations. These setters
// require exclusive control of all worlds and are only for deterministic fixtures.
namespace garden::ecs::testing {
struct ENGINE_API LifecycleTestAccess {
    static uint64_t exchangeWorldAllocator(uint64_t value) noexcept;
    static uint64_t exchangeEntitySerial(world& owner, uint64_t value) noexcept;
};
}
