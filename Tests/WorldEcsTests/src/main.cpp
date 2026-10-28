#include "world.hpp"
#include "ECS/ComponentOperations.hpp"
#include "ECS/EntityLifecycle.hpp"

#include <array>
#include <iostream>
#include <type_traits>
#include <utility>
#include <vector>

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
void check(bool condition, const char* name) {
    std::cout << (condition ? "[PASS] " : "[FAIL] ") << name << '\n';
    if (!condition) ++failures;
}

void basicAndReuse() {
    world a, b;
    const EntityRef ref = a.createEntity();
    const auto entity = a.scopedEntity(ref);
    const auto foreign = b.createScopedEntity();
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
          b.destroyEntity(entity) == ComponentStoreResult::InvalidEntity,
          "foreign world cannot access or destroy entity");
    check(a.remove<WorldValue>(entity) == ComponentStoreResult::Success &&
          a.remove<WorldValue>(entity) == ComponentStoreResult::NotFound &&
          a.get<WorldValue>(entity, value) == ComponentStoreResult::NotFound && !value &&
          a.has<WorldValue>(entity, present) == ComponentStoreResult::Success && !present,
          "removed component is absent");
    check(a.add<WorldValue>(entity, 11) == ComponentStoreResult::Success &&
          a.destroyEntity(entity) == ComponentStoreResult::Success && !a.isEntityValid(entity),
          "destroy invalidates entity and clears its value");
    const auto replacement = a.createScopedEntity();
    check(a.isEntityValid(replacement) && replacement != entity &&
          a.add<WorldValue>(replacement, 22) == ComponentStoreResult::Success &&
          a.get<WorldValue>(entity, value) == ComponentStoreResult::StaleEntity && !value &&
          a.destroyEntity(entity) == ComponentStoreResult::StaleEntity &&
          a.get<WorldValue>(replacement, value) == ComponentStoreResult::Success && value && value->number == 22,
          "replacement generation remains isolated from stale handle");
}

void identityBoundary() {
    world w, other;
    EntityLifecycle lifecycle(w.registry);
    const auto fresh = w.createScopedEntity();
    const world& read = w;
    const EntityRef freshRef = read.persistedReference(fresh);
    const auto freshRaw = resolveEntityRef(w.registry, freshRef);
    check(w.isEntityValid(fresh) && !freshRef.isNull() && freshRaw != entt::null &&
          w.registry.get<IdentityComponent>(freshRaw).guid == freshRef.guid &&
          lifecycle.resolve(freshRef) == freshRaw && w.scopedEntity(freshRef) == fresh,
          "fresh scoped entity has a unique persisted identity");

    const EntityRef requested{{0x123456789abcdef0ull, 0xfedcba9876543210ull}};
    const auto restored = w.restoreScopedEntity(requested);
    const auto raw = resolveEntityRef(w.registry, requested);
    check(w.isEntityValid(restored) && raw != entt::null &&
          w.registry.get<IdentityComponent>(raw).guid == requested.guid &&
          lifecycle.resolve(requested) == raw && read.persistedReference(restored) == requested &&
          w.scopedEntity(requested) == restored,
          "restore preserves the exact requested identity and indexes it");

    const auto count = w.registry.storage<entt::entity>().free_list();
    const auto index = lifecycle.index().by_guid;
    const auto duplicate = w.restoreScopedEntity(requested);
    const auto nullRestore = w.restoreScopedEntity(EntityRef{});
    check(!w.isEntityValid(duplicate) && !w.isEntityValid(nullRestore) &&
          w.registry.storage<entt::entity>().free_list() == count &&
          lifecycle.index().by_guid == index && lifecycle.resolve(requested) == raw &&
          w.registry.get<IdentityComponent>(raw).guid == requested.guid,
          "duplicate and null restoration leave entities and identity index unchanged");

    const auto foreign = other.createScopedEntity();
    check(read.persistedReference(restored) == requested &&
          read.persistedReference(EntityHandle{}).isNull() &&
          read.persistedReference(foreign).isNull() &&
          other.persistedReference(restored).isNull() &&
          w.registry.storage<entt::entity>().free_list() == count &&
          lifecycle.index().by_guid == index,
          "reference conversion is read-only and rejects null and foreign handles");

    // Corrupt identities directly to verify conversion does not repair them.
    const auto anonymous = w.registry.create();
    w.registry.emplace<IdentityComponent>(anonymous, IdentityComponent{requested.guid});
    const auto ambiguousIndex = lifecycle.index().by_guid;
    check(read.persistedReference(restored).isNull() &&
          w.registry.get<IdentityComponent>(anonymous).guid == requested.guid &&
          lifecycle.index().by_guid == ambiguousIndex,
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

    check(w.destroyEntity(restored) == ComponentStoreResult::Success &&
          read.persistedReference(restored).isNull(),
          "conversion rejects a stale handle");

    check(w.add<QueryValue>(fresh, QueryValue{7}) == ComponentStoreResult::Success,
          "prepare identity query guard");
    const EntityRef duringQuery{{0x31415926ull, 0x27182818ull}};
    const auto beforeQueryCount = w.registry.storage<entt::entity>().free_list();
    const auto beforeQueryIndex = lifecycle.index().by_guid;
    bool rejected = false;
    bool readSucceeded = false;
    const auto queryResult = w.query<QueryValue>([&](EntityHandle entity, QueryValue&) {
        rejected = !w.isEntityValid(w.restoreScopedEntity(duringQuery));
        readSucceeded = read.persistedReference(entity) == freshRef;
    });
    check(queryResult == ComponentStoreResult::Success && rejected && readSucceeded &&
          w.registry.storage<entt::entity>().free_list() == beforeQueryCount &&
          lifecycle.index().by_guid == beforeQueryIndex &&
          resolveEntityRef(w.registry, duringQuery) == entt::null,
          "query permits conversion and rejects restoration without mutation");

    const auto ambiguousHandle = w.createScopedEntity();
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

void cohortRestore() {
    world w;
    const EntityRef existingRef{{0xc001, 0xd001}};
    const auto existing = w.restoreScopedEntity(existingRef);
    const auto companion = w.createScopedEntity();
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

    const EntityRef duplicate{{0xc002, 0xd002}};
    const bool duplicateResult = w.restoreScopedEntities({duplicate, duplicate}, output);
    check(!duplicateResult && output == initialOutput && sentinelsIntact() &&
          !w.isEntityValid(w.scopedEntity(duplicate)),
          "cohort rejects duplicate requested GUID without publishing or changing outputs");

    const EntityRef collisionBefore{{0xc003, 0xd003}};
    const EntityRef collisionAfter{{0xc004, 0xd004}};
    const bool collisionResult = w.restoreScopedEntities({collisionBefore, existingRef, collisionAfter}, output);
    check(!collisionResult && output == initialOutput && sentinelsIntact() &&
          !w.isEntityValid(w.scopedEntity(collisionBefore)) &&
          !w.isEntityValid(w.scopedEntity(collisionAfter)),
          "cohort rejects destination collision before publishing any member");

    const EntityRef beforeNull{{0xc005, 0xd005}};
    const bool nullResult = w.restoreScopedEntities({beforeNull, EntityRef{}}, output);
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
            !w.restoreScopedEntities({duringQuery}, output);
    });
    check(queryResult == ComponentStoreResult::Success && queryVisits == 1 && queryRejected &&
          output == initialOutput && sentinelsIntact() &&
          !w.isEntityValid(w.scopedEntity(duringQuery)),
          "active query rejects cohort restore without changing outputs or identities");

    const EntityRef firstRef{{0xc007, 0xd007}};
    const EntityRef secondRef{{0xc008, 0xd008}};
    const bool success = w.restoreScopedEntities({firstRef, secondRef}, output);
    const bool ordered = success && output.size() == 2 && output[0] != output[1] &&
        w.isEntityValid(output[0]) && w.isEntityValid(output[1]) &&
        w.persistedReference(output[0]) == firstRef && w.persistedReference(output[1]) == secondRef &&
        w.scopedEntity(firstRef) == output[0] && w.scopedEntity(secondRef) == output[1];
    check(ordered && sentinelsIntact(),
          "successful cohort publishes exact requested identities in input order");
}

void moves() {
    world source;
    const auto original = source.createScopedEntity();
    check(source.add<WorldValue>(original, 31) == ComponentStoreResult::Success, "prepare move construction");
    world moved(std::move(source));
    WorldValue* value = nullptr;
    const auto fresh = source.createScopedEntity();
    check(moved.isEntityValid(original) && !source.isEntityValid(original) &&
          source.isEntityValid(fresh) && !moved.isEntityValid(fresh) &&
          moved.get<WorldValue>(original, value) == ComponentStoreResult::Success && value && value->number == 31,
          "move construction transfers handles and isolates moved-from world");
    world target;
    const auto oldTarget = target.createScopedEntity();
    check(target.add<WorldValue>(oldTarget, 99) == ComponentStoreResult::Success, "prepare move assignment");
    target = std::move(moved);
    const auto movedFresh = moved.createScopedEntity();
    check(target.isEntityValid(original) && !target.isEntityValid(oldTarget) &&
          !moved.isEntityValid(original) && moved.isEntityValid(movedFresh) &&
          target.get<WorldValue>(original, value) == ComponentStoreResult::Success && value && value->number == 31,
          "move assignment retires destination and transfers source handles");
}

void teardown() {
    world w;
    auto first = w.createScopedEntity();
    const int before = WorldValue::destroyed;
    check(w.add<WorldValue>(first, 1) == ComponentStoreResult::Success, "prepare shutdown");
    w.shutdown();
    WorldValue* expired = nullptr;
    check(!w.isEntityValid(first) && w.get<WorldValue>(first, expired) == ComponentStoreResult::InvalidEntity &&
          !expired && WorldValue::destroyed == before + 1,
          "shutdown expires handles and destroys values");
    auto second = w.createScopedEntity();
    check(w.isEntityValid(second) && w.add<WorldValue>(second, 2) == ComponentStoreResult::Success,
          "world can be reused after shutdown");
    w.resetWorld();
    check(!w.isEntityValid(second) && !w.isEntityValid(first) && WorldValue::destroyed == before + 2,
          "reset expires handles and destroys values");
    auto third = w.createScopedEntity();
    WorldValue* value = nullptr;
    check(w.isEntityValid(third) && w.get<WorldValue>(third, value) == ComponentStoreResult::NotFound && !value,
          "reset starts with empty component storage");
}

void queries() {
    world w;
    const auto both = w.createScopedEntity();
    const auto onlyWorld = w.createScopedEntity();
    const auto onlyQuery = w.createScopedEntity();
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
    ComponentStoreResult destroyResult = ComponentStoreResult::Success;
    bool scopedCreationRejected = false;
    bool creationRejected = false;
    const auto guarded = w.query<WorldValue, QueryValue>(
        [&](EntityHandle entity, WorldValue&, QueryValue&) {
            addResult = w.add<QueryValue>(onlyWorld, QueryValue{9});
            removeResult = w.remove<WorldValue>(entity);
            destroyResult = w.destroyEntity(onlyQuery);
            scopedCreationRejected = !w.isEntityValid(w.createScopedEntity());
            creationRejected = w.createEntity().isNull();
            w.shutdown();
            w.resetWorld();
        });
    bool present = false;
    QueryValue* queryValue = nullptr;
    check(guarded == ComponentStoreResult::Success &&
          addResult == ComponentStoreResult::StructuralMutationDuringQuery &&
          removeResult == ComponentStoreResult::StructuralMutationDuringQuery &&
          destroyResult == ComponentStoreResult::StructuralMutationDuringQuery &&
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
    const EntityRef ref = w.createEntity();
    const auto entity = w.scopedEntity(ref);
    const auto raw = resolveEntityRef(w.registry, ref);
    const auto foreign = other.createScopedEntity();
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

    const EntityRef secondRef = w.createEntity();
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
    check(w.destroyEntity(second) == ComponentStoreResult::Success &&
          w.get<TransformComponent>(second, transform) == ComponentStoreResult::StaleEntity && !transform &&
          w.add<PlayerComponent>(second) == ComponentStoreResult::StaleEntity,
          "adapter rejects stale handles");
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
    cohortRestore();
    moves();
    teardown();
    queries();
    engineAdapters();
    check(WorldValue::constructed == WorldValue::destroyed, "all component values destroyed exactly once");
    check(catalog.unregisterOwner(owner) == ComponentCatalogResult::Success, "worlds release catalog leases");
    return failures ? 1 : 0;
}
