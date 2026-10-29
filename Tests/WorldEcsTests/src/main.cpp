#include "world.hpp"
#include "ECS/ComponentOperations.hpp"
#include "Events/EventBus.hpp"

#include <array>
#include <iostream>
#include <type_traits>
#include <thread>
#include <utility>
#include <vector>

struct EventBusTestAccess {
    static void failNextCreatedBatchAllocation(EventBus& bus) noexcept { bus.failNextCreatedBatchAllocation = true; }
};

struct WorldValue {
    static inline int constructed = 0;
    static inline int destroyed = 0;
    int number;
    explicit WorldValue(int value) : number(value) { ++constructed; }
    WorldValue(const WorldValue&) = delete;
    ~WorldValue() noexcept { ++destroyed; }
};
struct UnregisteredValue { int number = 0; };
struct QueryValue { int number = 0; };

namespace garden::ecs {
template<> struct ComponentType<WorldValue> {
    static constexpr ComponentKey key{"garden.tests.WorldValue", 1};
};
template<> struct ComponentType<UnregisteredValue> {
    static constexpr ComponentKey key{"garden.tests.UnregisteredWorldValue", 1};
};
template<> struct ComponentType<QueryValue> {
    static constexpr ComponentKey key{"garden.tests.QueryValue", 1};
};
}

using namespace garden::ecs;
namespace {
int failures = 0;
world* reentrantWorld = nullptr;
ConstructionHandle reentrantHandle{};
ConstructionStatus reentrantStatus = ConstructionStatus::Completed;
ConstructionStatus reentrantJoinStatus = ConstructionStatus::Completed;
bool reentrantCapturesEmpty = false;
void probeReentrantCommit(entt::registry&, entt::entity) {
    const auto result = reentrantWorld->commitConstruction(reentrantHandle);
    reentrantStatus = result.status;
    reentrantCapturesEmpty = result.captured.empty() && result.entities.empty();
    reentrantJoinStatus = reentrantWorld->joinConstruction(reentrantHandle).status();
}
void corruptStagedIdentity(entt::registry& registry, entt::entity entity) {
    registry.get<IdentityComponent>(entity).guid = {};
}
bool failHierarchyOnce(void* context, LifecyclePhase phase) noexcept {
    auto& armed = *static_cast<bool*>(context);
    if (armed && phase == LifecyclePhase::Hierarchy) { armed = false; return true; }
    return false;
}
void check(bool condition, const char* name) {
    std::cout << (condition ? "[PASS] " : "[FAIL] ") << name << '\n';
    if (!condition) ++failures;
}


bool finishFixtureRollback(world& w, LifecycleResult rollback) {
    if (!rollback.operation.valid()) return true;
    const auto token = rollback.operation;
    for (int i = 0; i < 8 && !rollback.cleanupComplete(); ++i)
        rollback = w.resumeRetirement(token);
    if (!rollback.cleanupComplete()) { check(false, "fixture rollback completes"); return false; }
    const auto ack = w.acknowledgeRetirement(token);
    if (!ack.cleanupComplete()) { check(false, "fixture rollback acknowledgement"); return false; }
    return true;
}
EntityCreationResult createForTest(world& w, std::string_view tag = {}) {
    auto result = w.createScopedEntity(tag);
    const bool cleanup = finishFixtureRollback(w, result.rollback);
    if (result.status != ScopedRestoreStatus::Completed || !cleanup || result.rollback.operation.valid()) {
        check(false, "fixture entity creation succeeds");
        return {};
    }
    return result;
}
EntityCreationResult createRefForTest(world& w) {
    auto result = w.createEntity();
    const bool cleanup = finishFixtureRollback(w, result.rollback);
    if (result.status != ScopedRestoreStatus::Completed || !cleanup || result.rollback.operation.valid()) {
        check(false, "fixture reference creation succeeds");
        return {};
    }
    return result;
}

EntityHandle restoreForTest(world& w, EntityRef ref) {
    auto result = w.restoreScopedEntity(ref);
    if (!finishFixtureRollback(w, result.rollback)) return {};
    return result.status == ScopedRestoreStatus::Completed && !result.entities.empty() ? result.entities.front() : EntityHandle{};
}

void basicAndReuse() {
    world a, b;
    const EntityRef ref = createRefForTest(a).reference;
    const auto entity = a.scopedEntity(ref);
    const auto foreign = createForTest(b).entity;
    check(a.isEntityValid(entity) && a.scopedEntity(ref) == entity &&
          !b.isEntityValid(entity) && !a.isEntityValid(foreign) &&
          !a.isEntityValid(a.scopedEntity(EntityRef{})), "scope identity and reject foreign/null handles");

    WorldValue* value = nullptr;
    check(a.add<WorldValue>(entity, 42) == ComponentStoreResult::Success &&
          a.get<WorldValue>(entity, value) == ComponentStoreResult::Success && value && value->number == 42,
          "construct and retrieve component");
    const world& ca = a;
    const WorldValue* constValue = nullptr;
    check(ca.get<WorldValue>(entity, constValue) == ComponentStoreResult::Success &&
          constValue == value, "const retrieval sees stored value");
    bool present = false;
    check(ca.has<WorldValue>(entity, present) == ComponentStoreResult::Success && present,
          "typed membership reports stored component");
    check(a.add<WorldValue>(entity, 77) == ComponentStoreResult::Duplicate && value->number == 42,
          "duplicate leaves original value intact");
    UnregisteredValue* missing = reinterpret_cast<UnregisteredValue*>(1);
    check(a.get<UnregisteredValue>(entity, missing) == ComponentStoreResult::UnregisteredType && !missing &&
          a.add<UnregisteredValue>(entity) == ComponentStoreResult::UnregisteredType,
          "unregistered type is rejected and clears output");
    check(b.get<WorldValue>(entity, value) == ComponentStoreResult::InvalidEntity && !value &&
          b.add<WorldValue>(entity, 9) == ComponentStoreResult::InvalidEntity &&
          b.retireEntity(entity).status == LifecycleStatus::WrongWorld,
          "foreign world cannot access or destroy entity");
    check(a.remove<WorldValue>(entity) == ComponentStoreResult::Success &&
          a.remove<WorldValue>(entity) == ComponentStoreResult::NotFound &&
          a.get<WorldValue>(entity, value) == ComponentStoreResult::NotFound && !value &&
          a.has<WorldValue>(entity, present) == ComponentStoreResult::Success && !present,
          "removed component is absent");
    check(a.add<WorldValue>(entity, 11) == ComponentStoreResult::Success &&
          a.retireEntity(entity).cleanupComplete() && !a.isEntityValid(entity),
          "destroy invalidates entity and clears its value");
    const auto replacement = createForTest(a).entity;
    check(a.isEntityValid(replacement) && replacement != entity &&
          a.add<WorldValue>(replacement, 22) == ComponentStoreResult::Success &&
          a.get<WorldValue>(entity, value) == ComponentStoreResult::StaleEntity && !value &&
          a.retireEntity(entity).status == LifecycleStatus::Stale &&
          a.get<WorldValue>(replacement, value) == ComponentStoreResult::Success && value && value->number == 22,
          "replacement generation remains isolated from stale handle");
}

void identityBoundary() {
    world w, other;
    const auto fresh = createForTest(w).entity;
    const world& read = w;
    const EntityRef freshRef = read.persistedReference(fresh);
    const auto freshRaw = resolveEntityRef(w.registry, freshRef);
    check(w.isEntityValid(fresh) && !freshRef.isNull() && freshRaw != entt::null &&
          w.registry.get<IdentityComponent>(freshRaw).guid == freshRef.guid &&
          w.scopedEntity(freshRef) == fresh,
          "fresh scoped entity has a unique persisted identity");

    const EntityRef requested{{0x123456789abcdef0ull, 0xfedcba9876543210ull}};
    const auto restored = restoreForTest(w, requested);
    const auto raw = resolveEntityRef(w.registry, requested);
    check(w.isEntityValid(restored) && raw != entt::null &&
          w.registry.get<IdentityComponent>(raw).guid == requested.guid &&
          read.persistedReference(restored) == requested &&
          w.scopedEntity(requested) == restored,
          "restore preserves the exact requested identity");

    const auto count = w.registry.storage<entt::entity>().free_list();
    const auto duplicate = restoreForTest(w, requested);
    const auto nullRestore = restoreForTest(w, EntityRef{});
    check(!w.isEntityValid(duplicate) && !w.isEntityValid(nullRestore) &&
          w.registry.storage<entt::entity>().free_list() == count &&
          resolveEntityRef(w.registry, requested) == raw &&
          w.registry.get<IdentityComponent>(raw).guid == requested.guid,
          "duplicate and null restoration leave identities unchanged");

    const auto foreign = createForTest(other).entity;
    check(read.persistedReference(restored) == requested &&
          read.persistedReference(EntityHandle{}).isNull() &&
          read.persistedReference(foreign).isNull() &&
          other.persistedReference(restored).isNull() &&
          w.registry.storage<entt::entity>().free_list() == count,
          "reference conversion is read-only and rejects null and foreign handles");

    // Corrupt identities directly to verify conversion does not repair them.
    const auto anonymous = w.registry.create();
    w.registry.emplace<IdentityComponent>(anonymous, IdentityComponent{requested.guid});
    check(read.persistedReference(restored).isNull() &&
          w.registry.get<IdentityComponent>(anonymous).guid == requested.guid,
          "conversion rejects ambiguous identity without changing the registry");
    w.registry.destroy(anonymous);
    w.registry.get<IdentityComponent>(raw).guid = {};
    check(read.persistedReference(restored).isNull() &&
          w.registry.get<IdentityComponent>(raw).guid.isNull(),
          "conversion does not repair a null identity");
    w.registry.get<IdentityComponent>(raw).guid = requested.guid;
    w.registry.remove<IdentityComponent>(raw);
    check(read.persistedReference(restored).isNull() &&
          !w.registry.all_of<IdentityComponent>(raw),
          "conversion does not assign a missing identity");

    w.registry.emplace<IdentityComponent>(raw, IdentityComponent{requested.guid});
    const auto retired = w.retireEntity(restored);
    check(retired.status == LifecycleStatus::Completed &&
          read.persistedReference(restored).isNull(),
          "conversion rejects a stale handle");

    check(w.add<QueryValue>(fresh, QueryValue{7}) == ComponentStoreResult::Success,
          "prepare identity query guard");
    const EntityRef duringQuery{{0x31415926ull, 0x27182818ull}};
    const auto beforeQueryCount = w.registry.storage<entt::entity>().free_list();
    bool rejected = false;
    bool readSucceeded = false;
    const auto queryResult = w.query<QueryValue>([&](EntityHandle entity, QueryValue&) {
        rejected = !w.isEntityValid(restoreForTest(w, duringQuery));
        readSucceeded = read.persistedReference(entity) == freshRef;
    });
    check(queryResult == ComponentStoreResult::Success && rejected && readSucceeded &&
          w.registry.storage<entt::entity>().free_list() == beforeQueryCount &&
          resolveEntityRef(w.registry, duringQuery) == entt::null,
          "query permits conversion and rejects restoration without mutation");

    const auto ambiguousHandle = createForTest(w).entity;
    const EntityRef ambiguousRef = read.persistedReference(ambiguousHandle);
    const auto duplicateRaw = w.registry.create();
    w.registry.emplace<IdentityComponent>(duplicateRaw, IdentityComponent{ambiguousRef.guid});
    int outerVisits = 0;
    int innerVisits = 0;
    bool snapshotsValid = true;
    const auto nested = w.query<QueryValue>([&](EntityHandle outer, QueryValue&) {
        ++outerVisits;
        snapshotsValid = snapshotsValid && read.persistedReference(outer) == freshRef &&
                         read.persistedReference(ambiguousHandle).isNull();
        const auto inner = w.query<const QueryValue>([&](EntityHandle found, const QueryValue&) {
            ++innerVisits;
            snapshotsValid = snapshotsValid && read.persistedReference(found) == freshRef &&
                             read.persistedReference(ambiguousHandle).isNull();
        });
        snapshotsValid = snapshotsValid && inner == ComponentStoreResult::Success &&
                         read.persistedReference(outer) == freshRef &&
                         read.persistedReference(ambiguousHandle).isNull();
        const auto throwingInner = w.query<const QueryValue>([&](EntityHandle found, const QueryValue&) {
            snapshotsValid = snapshotsValid && read.persistedReference(found) == freshRef &&
                             read.persistedReference(ambiguousHandle).isNull();
            throw 17;
        });
        snapshotsValid = snapshotsValid && throwingInner == ComponentStoreResult::CallbackFailed &&
                         read.persistedReference(outer) == freshRef &&
                         read.persistedReference(ambiguousHandle).isNull();
    });
    check(nested == ComponentStoreResult::Success && outerVisits == 1 && innerVisits == 1 && snapshotsValid,
          "nested query conversion rejects ambiguity and restores the outer snapshot after success and exception");

    const auto throwingOuter = w.query<QueryValue>([&](EntityHandle found, QueryValue&) {
        snapshotsValid = snapshotsValid && read.persistedReference(found) == freshRef &&
                         read.persistedReference(ambiguousHandle).isNull();
        throw 17;
    });
    w.registry.destroy(duplicateRaw);
    check(throwingOuter == ComponentStoreResult::CallbackFailed && snapshotsValid &&
          read.persistedReference(fresh) == freshRef &&
          read.persistedReference(ambiguousHandle) == ambiguousRef,
          "throwing outer query releases the snapshot for subsequent identity conversion");
}

// Exercise construction hooks through the existing registry bridge. The marker
// type is private, so the fixture identifies the other storage on the newly
// constructed entity without exposing that type as a public API.
struct RestoreHookFixture {
    enum class Action { Duplicate, RemoveMarker, ReplaceMarker };
    static inline world* target = nullptr;
    static inline Action action = Action::Duplicate;
    static inline entt::entity operation = entt::null;
    static inline entt::entity sideEffect = entt::null;
    static inline EntityHandle replacement{};
    static inline bool fired = false;

    static void onIdentity(entt::registry& registry, entt::entity entity) {
        if (fired) return;
        fired = true;
        operation = entity;
        const EntityRef ref{registry.get<IdentityComponent>(entity).guid};
        if (action == Action::Duplicate) {
            sideEffect = registry.create();
            registry.emplace<IdentityComponent>(sideEffect, IdentityComponent{ref.guid});
            return;
        }
        auto* identityStorage = &registry.storage<IdentityComponent>();
        for (auto&& [id, storage] : registry.storage()) {
            (void)id;
            if (&storage != identityStorage && storage.contains(entity)) {
                storage.erase(entity);
                break;
            }
        }
        if (action == Action::ReplaceMarker) replacement = target->scopedEntity(ref);
    }
};

void singleRestoreHooks() {
    world w;
    const EntityRef existingRef{{0xe001, 0xf001}};
    const auto existing = restoreForTest(w, existingRef);
    const bool prepared = w.isEntityValid(existing) &&
        w.add<WorldValue>(existing, 73) == ComponentStoreResult::Success;
    check(prepared, "prepare single restore hook sentinel");
    if (!prepared) return;
    const auto sentinelIntact = [&]() {
        WorldValue* value = nullptr;
        return w.isEntityValid(existing) && w.persistedReference(existing) == existingRef &&
            w.get<WorldValue>(existing, value) == ComponentStoreResult::Success && value && value->number == 73;
    };
    auto sink = w.registry.on_construct<IdentityComponent>();
    sink.connect<&RestoreHookFixture::onIdentity>();
    RestoreHookFixture::target = &w;
    const auto run = [&](RestoreHookFixture::Action action, EntityRef ref) {
        RestoreHookFixture::action = action;
        RestoreHookFixture::fired = false;
        RestoreHookFixture::operation = entt::null;
        RestoreHookFixture::sideEffect = entt::null;
        RestoreHookFixture::replacement = {};
        return restoreForTest(w, ref);
    };
    const EntityRef duplicateRef{{0xe002, 0xf002}};
    const auto duplicate = run(RestoreHookFixture::Action::Duplicate, duplicateRef);
    const auto sideEffect = RestoreHookFixture::sideEffect;
    check(RestoreHookFixture::fired && !w.isEntityValid(duplicate) &&
          !w.registry.valid(RestoreHookFixture::operation) && w.registry.valid(sideEffect) &&
          w.registry.get<IdentityComponent>(sideEffect).guid == duplicateRef.guid && sentinelIntact(),
          "hook-created duplicate rejects restore, rolls back only the operation entity");
    // External hook effects remain; a second preexisting owner must also make
    // preflight fail even when GUID resolution rejects ambiguous identities.
    const auto secondOwner = w.registry.create();
    w.registry.emplace<IdentityComponent>(secondOwner, IdentityComponent{duplicateRef.guid});
    RestoreHookFixture::fired = false;
    const auto preexisting = restoreForTest(w, duplicateRef);
    check(!w.isEntityValid(preexisting) && !RestoreHookFixture::fired &&
          w.registry.valid(sideEffect) && w.registry.valid(secondOwner) && sentinelIntact(),
          "preflight rejects an already ambiguous GUID without changing its owners");
    w.registry.destroy(sideEffect);
    w.registry.destroy(secondOwner);

    const auto removed = run(RestoreHookFixture::Action::RemoveMarker, EntityRef{{0xe003, 0xf003}});
    check(RestoreHookFixture::fired && !w.isEntityValid(removed) &&
          !w.registry.valid(RestoreHookFixture::operation) && sentinelIntact(),
          "hook removing the scope marker rejects and rolls back restore");
    const auto replaced = run(RestoreHookFixture::Action::ReplaceMarker, EntityRef{{0xe004, 0xf004}});
    check(RestoreHookFixture::fired && !w.isEntityValid(replaced) &&
          !w.isEntityValid(RestoreHookFixture::replacement) &&
          !w.registry.valid(RestoreHookFixture::operation) && sentinelIntact(),
          "hook replacing the marker with a new serial rejects and rolls back restore");
    sink.disconnect<&RestoreHookFixture::onIdentity>();
    RestoreHookFixture::target = nullptr;
}

void cohortRestore() {
    world w;
    const EntityRef existingRef{{0xc001, 0xd001}};
    const auto existing = restoreForTest(w, existingRef);
    const auto companion = createForTest(w).entity;
    const EntityRef companionRef = w.persistedReference(companion);
    const bool prepared = w.isEntityValid(existing) && w.isEntityValid(companion) &&
        !companionRef.isNull() &&
        w.add<WorldValue>(existing, 11) == ComponentStoreResult::Success &&
        w.add<WorldValue>(companion, 22) == ComponentStoreResult::Success &&
        w.add<QueryValue>(existing, QueryValue{7}) == ComponentStoreResult::Success;
    check(prepared, "prepare cohort restore sentinels");
    if (!prepared) return;

    const std::vector<EntityHandle> initialOutput{companion, existing};
    std::vector<EntityHandle> output = initialOutput;
    const auto sentinelsIntact = [&]() {
        WorldValue* first = nullptr;
        WorldValue* second = nullptr;
        return w.isEntityValid(existing) && w.isEntityValid(companion) &&
            w.persistedReference(existing) == existingRef &&
            w.persistedReference(companion) == companionRef &&
            w.scopedEntity(existingRef) == existing && w.scopedEntity(companionRef) == companion &&
            w.get<WorldValue>(existing, first) == ComponentStoreResult::Success && first && first->number == 11 &&
            w.get<WorldValue>(companion, second) == ComponentStoreResult::Success && second && second->number == 22;
    };

    const auto restoreCohort = [&](std::initializer_list<EntityRef> refs, std::vector<EntityHandle>& out) {
        auto result = w.restoreScopedEntities(std::span<const EntityRef>(refs.begin(), refs.size()));
        if (!finishFixtureRollback(w, result.rollback)) return false;
        if (result.status != ScopedRestoreStatus::Completed) return false;
        out = std::move(result.entities); return true;
    };
    const EntityRef duplicate{{0xc002, 0xd002}};
    const bool duplicateResult = restoreCohort({duplicate, duplicate}, output);
    check(!duplicateResult && output == initialOutput && sentinelsIntact() &&
          !w.isEntityValid(w.scopedEntity(duplicate)),
          "cohort rejects duplicate requested GUID without publishing or changing outputs");

    const EntityRef collisionBefore{{0xc003, 0xd003}};
    const EntityRef collisionAfter{{0xc004, 0xd004}};
    const bool collisionResult = restoreCohort({collisionBefore, existingRef, collisionAfter}, output);
    check(!collisionResult && output == initialOutput && sentinelsIntact() &&
          !w.isEntityValid(w.scopedEntity(collisionBefore)) &&
          !w.isEntityValid(w.scopedEntity(collisionAfter)),
          "cohort rejects destination collision before publishing any member");

    const EntityRef beforeNull{{0xc005, 0xd005}};
    const bool nullResult = restoreCohort({beforeNull, EntityRef{}}, output);
    check(!nullResult && output == initialOutput && sentinelsIntact() &&
          !w.isEntityValid(w.scopedEntity(beforeNull)),
          "cohort rejects a null GUID without publishing or changing outputs");

    const EntityRef duringQuery{{0xc006, 0xd006}};
    bool queryRejected = false;
    int queryVisits = 0;
    const auto queryResult = w.query<const QueryValue>([&](EntityHandle entity, const QueryValue& value) {
        ++queryVisits;
        queryRejected = entity == existing && value.number == 7 &&
            w.persistedReference(entity) == existingRef &&
            !restoreCohort({duringQuery}, output);
    });
    check(queryResult == ComponentStoreResult::Success && queryVisits == 1 && queryRejected &&
          output == initialOutput && sentinelsIntact() &&
          !w.isEntityValid(w.scopedEntity(duringQuery)),
          "active query rejects cohort restore without changing outputs or identities");

    const EntityRef firstRef{{0xc007, 0xd007}};
    const EntityRef secondRef{{0xc008, 0xd008}};
    const bool success = restoreCohort({firstRef, secondRef}, output);
    const bool ordered = success && output.size() == 2 && output[0] != output[1] &&
        w.isEntityValid(output[0]) && w.isEntityValid(output[1]) &&
        w.persistedReference(output[0]) == firstRef && w.persistedReference(output[1]) == secondRef &&
        w.scopedEntity(firstRef) == output[0] && w.scopedEntity(secondRef) == output[1];
    check(ordered && sentinelsIntact(),
          "successful cohort publishes exact requested identities in input order");
}

static_assert(!std::is_copy_constructible_v<world> && !std::is_copy_assignable_v<world>);
static_assert(!std::is_move_constructible_v<world> && !std::is_move_assignable_v<world>);

void teardown() {
    world w;
    auto first = createForTest(w).entity;
    const int before = WorldValue::destroyed;
    check(w.add<WorldValue>(first, 1) == ComponentStoreResult::Success, "prepare shutdown");
    w.shutdown();
    WorldValue* expired = nullptr;
    check(!w.isEntityValid(first) && w.get<WorldValue>(first, expired) == ComponentStoreResult::InvalidEntity &&
          !expired && WorldValue::destroyed == before + 1,
          "shutdown expires handles and destroys values");
    auto second = createForTest(w).entity;
    check(w.isEntityValid(second) && w.add<WorldValue>(second, 2) == ComponentStoreResult::Success,
          "world can be reused after shutdown");
    w.resetWorld();
    check(!w.isEntityValid(second) && !w.isEntityValid(first) && WorldValue::destroyed == before + 2,
          "reset expires handles and destroys values");
    auto third = createForTest(w).entity;
    WorldValue* value = nullptr;
    check(w.isEntityValid(third) && w.get<WorldValue>(third, value) == ComponentStoreResult::NotFound && !value,
          "reset starts with empty component storage");
}

void queries() {
    world w;
    const auto both = createForTest(w).entity;
    const auto onlyWorld = createForTest(w).entity;
    const auto onlyQuery = createForTest(w).entity;
    check(w.add<WorldValue>(both, 10) == ComponentStoreResult::Success &&
          w.add<QueryValue>(both, QueryValue{3}) == ComponentStoreResult::Success &&
          w.add<WorldValue>(onlyWorld, 20) == ComponentStoreResult::Success &&
          w.add<QueryValue>(onlyQuery, QueryValue{7}) == ComponentStoreResult::Success,
          "prepare joined query components");

    int visits = 0;
    bool matched = true;
    const auto joined = w.query<WorldValue, const QueryValue>(
        [&](EntityHandle entity, WorldValue& value, const QueryValue& increment) {
            ++visits;
            matched = matched && entity == both && value.number == 10 && increment.number == 3;
            value.number += increment.number;
        });
    WorldValue* value = nullptr;
    check(joined == ComponentStoreResult::Success && visits == 1 && matched &&
          w.get<WorldValue>(both, value) == ComponentStoreResult::Success && value && value->number == 13 &&
          w.get<WorldValue>(onlyWorld, value) == ComponentStoreResult::Success && value && value->number == 20,
          "mixed-owner joined query visits only matches and persists mutable writes");

    visits = 0;
    const auto read = w.query<const WorldValue, const QueryValue>(
        [&](EntityHandle entity, auto& v, auto& q) {
            static_assert(std::is_const_v<std::remove_reference_t<decltype(v)>>);
            static_assert(std::is_const_v<std::remove_reference_t<decltype(q)>>);
            ++visits;
            matched = matched && entity == both && v.number == 13 && q.number == 3;
        });
    check(read == ComponentStoreResult::Success && visits == 1 && matched,
          "const query yields read-only component references");

    visits = 0;
    const auto nested = w.query<WorldValue>([&](EntityHandle, WorldValue&) {
        const auto inner = w.query<const QueryValue>([&](EntityHandle, const QueryValue&) { ++visits; });
        matched = matched && inner == ComponentStoreResult::Success;
    });
    check(nested == ComponentStoreResult::Success && visits == 4 && matched,
          "nested queries visit inner matches for each outer match");

    const auto callbackFailure = w.query<WorldValue>([](EntityHandle, WorldValue&) { throw 17; });
    check(callbackFailure == ComponentStoreResult::CallbackFailed &&
          w.remove<WorldValue>(onlyWorld) == ComponentStoreResult::Success &&
          w.add<WorldValue>(onlyWorld, 20) == ComponentStoreResult::Success,
          "callback exceptions report failure and release the query guard");

    world empty;
    visits = 0;
    const auto emptyResult = empty.query<WorldValue, QueryValue>(
        [&](EntityHandle, WorldValue&, QueryValue&) { ++visits; });
    const auto missingResult = w.query<WorldValue, UnregisteredValue>(
        [&](EntityHandle, WorldValue&, UnregisteredValue&) { ++visits; });
    check(emptyResult == ComponentStoreResult::Success && missingResult == ComponentStoreResult::UnregisteredType && visits == 0,
          "empty registered pools and unregistered types do not invoke callbacks");

    ComponentStoreResult addResult = ComponentStoreResult::Success;
    ComponentStoreResult removeResult = ComponentStoreResult::Success;
    LifecycleStatus destroyResult = LifecycleStatus::Completed;
    bool scopedCreationRejected = false;
    bool creationRejected = false;
    LifecycleStatus shutdownResult = LifecycleStatus::Completed;
    LifecycleStatus resetResult = LifecycleStatus::Completed;
    const auto guarded = w.query<WorldValue, QueryValue>(
        [&](EntityHandle entity, WorldValue&, QueryValue&) {
            addResult = w.add<QueryValue>(onlyWorld, QueryValue{9});
            removeResult = w.remove<WorldValue>(entity);
            destroyResult = w.retireEntity(onlyQuery).status;
            scopedCreationRejected = w.createScopedEntity().status == ScopedRestoreStatus::QueryActive;
            creationRejected = w.createEntity().status == ScopedRestoreStatus::QueryActive;
            shutdownResult = w.shutdown().status;
            resetResult = w.resetWorld().status;
        });
    bool present = false;
    QueryValue* queryValue = nullptr;
    check(guarded == ComponentStoreResult::Success &&
          addResult == ComponentStoreResult::StructuralMutationDuringQuery &&
          removeResult == ComponentStoreResult::StructuralMutationDuringQuery &&
          destroyResult == LifecycleStatus::QueryActive &&
          shutdownResult == LifecycleStatus::QueryActive && resetResult == LifecycleStatus::QueryActive &&
          scopedCreationRejected && creationRejected &&
          w.isEntityValid(both) && w.isEntityValid(onlyWorld) && w.isEntityValid(onlyQuery) &&
          w.has<QueryValue>(onlyWorld, present) == ComponentStoreResult::Success && !present &&
          w.get<WorldValue>(both, value) == ComponentStoreResult::Success && value && value->number == 13 &&
          w.get<WorldValue>(onlyWorld, value) == ComponentStoreResult::Success && value && value->number == 20 &&
          w.get<QueryValue>(onlyQuery, queryValue) == ComponentStoreResult::Success && queryValue && queryValue->number == 7,
          "query rejects structural mutations without changing state");
}

void engineAdapters() {
    world w, other;
    const EntityRef ref = createRefForTest(w).reference;
    const auto entity = w.scopedEntity(ref);
    const auto raw = resolveEntityRef(w.registry, ref);
    const auto foreign = createForTest(other).entity;
    check(raw != entt::null && w.add<TransformComponent>(entity, 2.0f, 3.0f, 4.0f) == ComponentStoreResult::Success &&
          w.add<PlayerComponent>(entity) == ComponentStoreResult::Success,
          "Garden adds engine components to adapter pools");
    TransformComponent* transform = nullptr;
    PlayerComponent* player = nullptr;
    bool present = false;
    check(w.get<TransformComponent>(entity, transform) == ComponentStoreResult::Success && transform &&
          transform == w.registry.try_get<TransformComponent>(raw) && transform->position.x == 2.0f &&
          w.get<PlayerComponent>(entity, player) == ComponentStoreResult::Success && player &&
          player == w.registry.try_get<PlayerComponent>(raw) &&
          w.has<PlayerComponent>(entity, present) == ComponentStoreResult::Success && present &&
          w.add<TransformComponent>(entity) == ComponentStoreResult::Duplicate,
          "Garden access and registry access share authoritative engine values");
    if (transform && player) {
        transform->position.x = 8.0f;
        player->speed = 5.0f;
    }
    const world& cw = w;
    const TransformComponent* readTransform = nullptr;
    check(cw.get<TransformComponent>(entity, readTransform) == ComponentStoreResult::Success &&
          readTransform == w.registry.try_get<TransformComponent>(raw) && readTransform &&
          readTransform->position.x == 8.0f && w.registry.get<PlayerComponent>(raw).speed == 5.0f,
          "const access and registry observe Garden writes");

    const EntityRef secondRef = createRefForTest(w).reference;
    const auto second = w.scopedEntity(secondRef);
    const auto secondRaw = resolveEntityRef(w.registry, secondRef);
    w.registry.emplace<TransformComponent>(secondRaw, 12.0f, 0.0f, 0.0f);
    w.registry.emplace<PlayerComponent>(secondRaw);
    check(w.get<TransformComponent>(second, transform) == ComponentStoreResult::Success &&
          transform == w.registry.try_get<TransformComponent>(secondRaw) && transform && transform->position.x == 12.0f &&
          w.add<QueryValue>(entity, QueryValue{3}) == ComponentStoreResult::Success,
          "registry-created engine components are visible to Garden");
    int visits = 0;
    bool matched = true;
    ComponentStoreResult guarded = ComponentStoreResult::Success;
    const auto joined = w.query<TransformComponent, PlayerComponent, const QueryValue>(
        [&](EntityHandle found, TransformComponent& t, PlayerComponent& p, const QueryValue& module) {
            ++visits;
            matched = matched && found == entity && &t == w.registry.try_get<TransformComponent>(raw) &&
                      &p == w.registry.try_get<PlayerComponent>(raw) && module.number == 3;
            t.position.x += module.number;
            p.speed += module.number;
            guarded = w.remove<PlayerComponent>(found);
        });
    check(joined == ComponentStoreResult::Success && visits == 1 && matched &&
          guarded == ComponentStoreResult::StructuralMutationDuringQuery &&
          w.registry.get<TransformComponent>(raw).position.x == 11.0f &&
          w.registry.get<PlayerComponent>(raw).speed == 8.0f &&
          w.get<TransformComponent>(entity, transform) == ComponentStoreResult::Success &&
          transform == w.registry.try_get<TransformComponent>(raw) && transform->position.x == 11.0f,
          "engine and module pools join with shared writes and mutation guard");

    // Raw entities without an existing scope, including those with no identity,
    // must not be assigned identity or scope merely by a query.
    const auto anonymous = w.registry.create();
    w.registry.emplace<TransformComponent>(anonymous);
    const auto unscoped = w.registry.create();
    const EntityGuid unscopedGuid = generateEntityGuid();
    w.registry.emplace<IdentityComponent>(unscoped, IdentityComponent{unscopedGuid});
    w.registry.emplace<TransformComponent>(unscoped);
    const auto duplicate = w.registry.create();
    w.registry.emplace<IdentityComponent>(duplicate, IdentityComponent{ref.guid});
    w.registry.emplace<TransformComponent>(duplicate);
    visits = 0;
    matched = true;
    const auto invalidQuery = w.query<const TransformComponent>([&](EntityHandle found, const TransformComponent&) {
        ++visits;
        matched = matched && found == second;
    });
    check(invalidQuery == ComponentStoreResult::Success && visits == 1 && matched &&
          !w.registry.all_of<IdentityComponent>(anonymous) &&
          w.registry.get<IdentityComponent>(unscoped).guid == unscopedGuid &&
          w.registry.get<IdentityComponent>(duplicate).guid == ref.guid,
          "query skips unscoped and ambiguous identities without repairing them");
    w.registry.destroy(duplicate);
    const auto nullIdentity = w.registry.get<IdentityComponent>(raw).guid;
    w.registry.get<IdentityComponent>(raw).guid = {};
    visits = 0;
    matched = true;
    const auto nullQuery = w.query<const TransformComponent>([&](EntityHandle found, const TransformComponent&) {
        ++visits;
        matched = matched && found == second;
    });
    check(nullQuery == ComponentStoreResult::Success && visits == 1 && matched &&
          w.registry.get<IdentityComponent>(raw).guid.isNull(),
          "query skips null identities without repairing them");
    w.registry.get<IdentityComponent>(raw).guid = nullIdentity;

    check(w.get<TransformComponent>(foreign, transform) == ComponentStoreResult::InvalidEntity && !transform &&
          w.add<PlayerComponent>(EntityHandle{}) == ComponentStoreResult::InvalidEntity &&
          w.remove<PlayerComponent>(foreign) == ComponentStoreResult::InvalidEntity,
          "adapter rejects foreign and unscoped handles");
    check(w.remove<PlayerComponent>(entity) == ComponentStoreResult::Success &&
          !w.registry.all_of<PlayerComponent>(raw) &&
          w.remove<PlayerComponent>(entity) == ComponentStoreResult::NotFound &&
          w.has<PlayerComponent>(entity, present) == ComponentStoreResult::Success && !present &&
          w.remove<TransformComponent>(entity) == ComponentStoreResult::Success &&
          !w.registry.all_of<TransformComponent>(raw) &&
          w.get<TransformComponent>(entity, transform) == ComponentStoreResult::NotFound && !transform,
          "removing adapter values removes the registry values");
    check(w.retireEntity(second).cleanupComplete() &&
          w.get<TransformComponent>(second, transform) == ComponentStoreResult::StaleEntity && !transform &&
          w.add<PlayerComponent>(second) == ComponentStoreResult::StaleEntity,
          "adapter rejects stale handles");
}
void stagedConstruction(ComponentOwnerId owner) {
    world w;
    const auto existing = createForTest(w).entity;
    check(w.add<TransformComponent>(existing) == ComponentStoreResult::Success, "setup existing component");
    const auto begin = w.beginConstruction();
    check(begin.status == ConstructionStatus::Completed && begin.transaction.valid(), "begin staged construction");
    check(w.beginConstruction().status == ConstructionStatus::Conflict, "one active construction");
    world other;
    check(other.constructionStatus(begin.transaction) == ConstructionStatus::WrongWorld, "foreign transaction rejected");
    ConstructionStatus threadStatus = ConstructionStatus::Completed;
    std::thread worker([&] { threadStatus = w.constructionStatus(begin.transaction); }); worker.join();
    check(threadStatus == ConstructionStatus::WrongThread, "wrong thread rejected");
    const auto first = w.createStagedEntity(begin.transaction, "hidden");
    const EntityRef saved{generateEntityGuid()};
    StagedEntityResult second{};
    {
        auto join = w.joinConstruction(begin.transaction);
        auto moved = std::move(join);
        auto nested = w.joinConstruction(begin.transaction);
        second = w.restoreStagedEntity(begin.transaction, saved);
        const auto commit = w.commitConstruction(begin.transaction);
        const auto abort = w.abortConstruction(begin.transaction);
        check(moved.status() == ConstructionStatus::Completed && join.status() == ConstructionStatus::InvalidInput &&
              nested.status() == ConstructionStatus::Completed && w.beginConstruction().status == ConstructionStatus::Conflict &&
              commit.status == ConstructionStatus::Conflict && commit.entities.empty() && commit.captured.empty() &&
              abort.status == ConstructionStatus::Conflict && abort.captured.empty() && !abort.rollback.operation.valid(),
              "nested joins share transaction and reject independent begin and terminal operations");
    }
    check(first.status == ConstructionStatus::Completed && second.status == ConstructionStatus::Completed &&
          w.restoreStagedEntity(begin.transaction, saved).status == ConstructionStatus::Conflict,
          "stage fresh and restored identities and reserve duplicate");
    const auto otherTx = other.beginConstruction();
    check(w.joinConstruction(otherTx.transaction).status() == ConstructionStatus::WrongWorld &&
          w.joinConstruction(ConstructionHandle{}).status() == ConstructionStatus::InvalidInput,
          "join validates foreign and invalid transaction");
    ConstructionStatus threadJoin = ConstructionStatus::Completed;
    std::thread wrongJoinThread([&] { threadJoin = w.joinConstruction(begin.transaction).status(); });
    wrongJoinThread.join();
    check(threadJoin == ConstructionStatus::WrongThread, "join rejects wrong thread");
    const auto foreignCommit = w.commitConstruction(otherTx.transaction);
    ConstructionCommitResult wrongThreadCommit{};
    std::thread wrongCommitThread([&] { wrongThreadCommit = w.commitConstruction(begin.transaction); });
    wrongCommitThread.join();
    check(foreignCommit.status == ConstructionStatus::WrongWorld && foreignCommit.captured.empty() &&
          wrongThreadCommit.status == ConstructionStatus::WrongThread && wrongThreadCommit.captured.empty(),
          "foreign and wrong-thread commits do not expose captures");
    const auto otherMember = other.restoreStagedEntity(otherTx.transaction, saved);
    const auto otherCommit = other.commitConstruction(otherTx.transaction);
    check(otherMember.status == ConstructionStatus::Completed && otherCommit.status == ConstructionStatus::Completed &&
          otherCommit.entities.size() == 1 && otherCommit.entities[0] != second.captured,
          "same identity independently restored in another world");
    check(w.add<WorldValue>(existing, 9) != ComponentStoreResult::Success &&
          w.remove<TransformComponent>(existing) != ComponentStoreResult::Success,
          "transaction rejects ordinary structural edits to published entities");
    check(w.shutdown().status == LifecycleStatus::Conflict && w.resetWorld().status == LifecycleStatus::Conflict,
          "shutdown and reset reject active transaction");
    check(w.add<QueryValue>(begin.transaction, first.entity, QueryValue{8}).component_status == ComponentStoreResult::Success &&
          ComponentCatalog::instance().unregisterOwner(owner) == ComponentCatalogResult::InUse,
          "staged store-backed value retains provider lease");
    QueryValue* invalidOut = reinterpret_cast<QueryValue*>(1); bool invalidHas = true;
    check(w.get<QueryValue>(otherTx.transaction, first.entity, invalidOut).transaction_status != ConstructionStatus::Completed && !invalidOut &&
          w.has<QueryValue>(otherTx.transaction, first.entity, invalidHas).transaction_status != ConstructionStatus::Completed && !invalidHas,
          "foreign capabilities clear lookup outputs");
    ConstructionStatus queryStatus = ConstructionStatus::Completed;
    ConstructionStatus queryJoinStatus = ConstructionStatus::Completed;
    bool queryCapturesEmpty = false;
    w.query<TransformComponent>([&](EntityHandle, TransformComponent&) {
        const auto rejected = w.commitConstruction(begin.transaction);
        queryStatus = rejected.status; queryCapturesEmpty = rejected.captured.empty();
        queryJoinStatus = w.joinConstruction(begin.transaction).status();
    });
    check(queryStatus == ConstructionStatus::QueryActive && queryCapturesEmpty && queryJoinStatus == ConstructionStatus::QueryActive, "active query rejects commit and join without exposing captures");
    bool hasValue = false;
    reentrantWorld = &w; reentrantHandle = begin.transaction;
    w.registry.on_construct<TransformComponent>().connect<&probeReentrantCommit>();
    const auto added = w.add<TransformComponent>(begin.transaction, first.entity);
    w.registry.on_construct<TransformComponent>().disconnect<&probeReentrantCommit>();
    check(reentrantStatus == ConstructionStatus::Reentrant && reentrantJoinStatus == ConstructionStatus::Reentrant && reentrantCapturesEmpty,
          "reentrant commit does not expose captures");
    const auto has = w.has<TransformComponent>(begin.transaction, first.entity, hasValue);
    TransformComponent* value = nullptr;
    check(added.component_status == ComponentStoreResult::Success && has.component_status == ComponentStoreResult::Success && hasValue &&
          w.get<TransformComponent>(begin.transaction, first.entity, value).component_status == ComponentStoreResult::Success && value,
          "staged typed operations use engine adapter");
    TransformComponent* ordinary = nullptr;
    size_t seen = 0;
    w.query<TransformComponent>([&](EntityHandle, TransformComponent&) { ++seen; });
    check(!w.isEntityValid(first.captured) && w.scopedEntity(first.reference) == EntityHandle{} &&
          w.persistedReference(first.captured).isNull() && w.findEntityByTag("hidden").isNull() && seen == 1 &&
          w.get<TransformComponent>(first.captured, ordinary) != ComponentStoreResult::Success,
          "staged members hidden from ordinary garden operations");
    check(w.resolveStagedReference(begin.transaction, saved).entity == second.entity, "resolve staged reference");
    EventBusTestAccess::failNextCreatedBatchAllocation(EventBus::get());
    const auto failedCommit = w.commitConstruction(begin.transaction);
    check(failedCommit.status == ConstructionStatus::EventQueueFailure && failedCommit.captured.size() == 2 &&
          w.constructionStatus(begin.transaction) == ConstructionStatus::Completed &&
          w.resolveStagedReference(begin.transaction, saved).status == StagedLookupStatus::Found,
          "event queue failure preserves transaction for commit retry");
    const auto committed = w.commitConstruction(begin.transaction);
    check(committed.status == ConstructionStatus::Completed && committed.entities.size() == 2 &&
          committed.entities[0] == first.captured && committed.entities[1] == second.captured &&
          w.isEntityValid(first.captured) && w.isEntityValid(second.captured), "commit publishes ordered cohort");
    check(w.constructionStatus(begin.transaction) == ConstructionStatus::StaleTransaction, "committed token stale");
    check(w.joinConstruction(begin.transaction).status() == ConstructionStatus::StaleTransaction,
          "join rejects stale transaction");
    const auto next = w.beginConstruction();
    const auto abandoned = w.createStagedEntity(next.transaction);
    const auto staleCommit = w.commitConstruction(begin.transaction);
    const auto invalidCommit = w.commitConstruction(ConstructionHandle{});
    check(staleCommit.status == ConstructionStatus::StaleTransaction && staleCommit.captured.empty() &&
          invalidCommit.status == ConstructionStatus::InvalidInput && invalidCommit.captured.empty(),
          "stale and invalid commits do not expose later transaction captures");
    bool armed = true;
    w.registry.ctx().emplace<testing::LifecycleFaultHook>(testing::LifecycleFaultHook{&armed, &failHierarchyOnce});
    const auto aborted = w.abortConstruction(next.transaction);
    check(aborted.rollback.status == LifecycleStatus::Partial && aborted.captured.size() == 1,
          "partial abort retains captures for retry");
    w.registry.ctx().erase<testing::LifecycleFaultHook>();
    const auto resumed = w.resumeRetirement(aborted.rollback.operation);
    check(aborted.status == ConstructionStatus::Completed && resumed.cleanupComplete() &&
          aborted.captured.size() == 1 && aborted.captured.front() == abandoned.captured &&
          !w.isEntityValid(abandoned.captured) && aborted.rollback.operation.valid(), "abort retains exact captures and rollback operation");
    check(w.beginConstruction().status == ConstructionStatus::Conflict, "abort requires acknowledgement");
    LifecycleResult wrongResume, wrongAck, wrongShutdown, wrongReset;
    std::thread wrongThread([&] { wrongResume = w.resumeRetirement(aborted.rollback.operation);
        wrongAck = w.acknowledgeRetirement(aborted.rollback.operation);
        wrongShutdown = w.shutdown(); wrongReset = w.resetWorld(); });
    wrongThread.join();
    check(wrongResume.status == LifecycleStatus::WrongThread && wrongAck.status == LifecycleStatus::WrongThread &&
          wrongShutdown.status == LifecycleStatus::WrongThread && wrongReset.status == LifecycleStatus::WrongThread &&
          aborted.captured.front() == abandoned.captured, "wrong thread cannot resume, acknowledge, shutdown or reset");
    check(w.resumeRetirement(aborted.rollback.operation).cleanupComplete() && aborted.captured.front() == abandoned.captured,
          "terminal retry preserves capture span");
    check(w.acknowledgeRetirement(aborted.rollback.operation).cleanupComplete(), "acknowledge staged rollback");
    check(w.acknowledgeRetirement(aborted.rollback.operation).status == LifecycleStatus::UnknownOperation &&
          w.constructionStatus(next.transaction) == ConstructionStatus::StaleTransaction,
          "acknowledged token and staged transaction are stale");
    const auto poisoned = w.beginConstruction();
    const auto victim = w.createStagedEntity(poisoned.transaction);
    TransformComponent* staleOut = reinterpret_cast<TransformComponent*>(1);
    check(w.get<TransformComponent>(poisoned.transaction, first.entity, staleOut).transaction_status == ConstructionStatus::Conflict && !staleOut,
          "stale member capability cannot access later transaction");
    ConstructionComponentResult poisonedAdd{};
    {
        auto join = w.joinConstruction(poisoned.transaction);
        w.registry.on_construct<TransformComponent>().connect<&corruptStagedIdentity>();
        poisonedAdd = w.add<TransformComponent>(poisoned.transaction, victim.entity);
        w.registry.on_construct<TransformComponent>().disconnect<&corruptStagedIdentity>();
        const auto innerCommit = w.commitConstruction(poisoned.transaction);
        const auto innerAbort = w.abortConstruction(poisoned.transaction);
        check(join.status() == ConstructionStatus::Completed &&
              w.constructionStatus(poisoned.transaction) == ConstructionStatus::TransactionFailed &&
              innerCommit.status == ConstructionStatus::Conflict && innerCommit.captured.empty() &&
              innerAbort.status == ConstructionStatus::Conflict && innerAbort.captured.empty(),
              "joined failure propagates and inner terminal operations expose no captures");
    }
    const auto poisonedCommit = w.commitConstruction(poisoned.transaction);
    check(poisonedAdd.transaction_status == ConstructionStatus::TransactionFailed &&
          poisonedCommit.status == ConstructionStatus::TransactionFailed && poisonedCommit.captured.size() == 1 &&
          poisonedCommit.captured.front() == victim.captured,
          "callback identity tampering poisons add and blocks commit");
    const auto poisonAbort = w.abortConstruction(poisoned.transaction);
    check(finishFixtureRollback(w, poisonAbort.rollback), "poisoned construction aborts safely");
}

}

int main() {
    auto& catalog = ComponentCatalog::instance();
    std::array<uint8_t, 32> digest{};
    digest[0] = 0xe1;
    const auto owner = ComponentOwnerId::module(digest, 1);
    garden::ecs::ComponentDescriptor descriptor{ComponentType<WorldValue>::key, ComponentOwnerId::engineCore(), sizeof(WorldValue), alignof(WorldValue),
        componentCapabilities<WorldValue>(), makeComponentOperations<WorldValue>()};
    ComponentId id{};
    const auto registration = catalog.registerComponent(descriptor, id);
    check(registration == ComponentCatalogResult::Success, "register component metadata");
    if (registration != ComponentCatalogResult::Success) return 1;
    garden::ecs::ComponentDescriptor queryDescriptor{ComponentType<QueryValue>::key, owner, sizeof(QueryValue), alignof(QueryValue),
        componentCapabilities<QueryValue>(), makeComponentOperations<QueryValue>()};
    ComponentId queryId{};
    const auto queryRegistration = catalog.registerComponent(queryDescriptor, queryId);
    check(queryRegistration == ComponentCatalogResult::Success, "register query component metadata");
    if (queryRegistration != ComponentCatalogResult::Success) return 1;
    basicAndReuse();
    identityBoundary();
    singleRestoreHooks();
    cohortRestore();
    teardown();
    queries();
    engineAdapters();
    stagedConstruction(owner);
    check(WorldValue::constructed == WorldValue::destroyed, "all component values destroyed exactly once");
    check(catalog.unregisterOwner(owner) == ComponentCatalogResult::Success, "worlds release catalog leases");
    return failures ? 1 : 0;
}
