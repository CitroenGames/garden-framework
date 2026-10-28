#include "world.hpp"
#include "ECS/ComponentOperations.hpp"

#include <array>
#include <iostream>
#include <utility>

struct WorldValue {
    static inline int constructed = 0;
    static inline int destroyed = 0;
    int number;
    explicit WorldValue(int value) : number(value) { ++constructed; }
    WorldValue(const WorldValue&) = delete;
    ~WorldValue() noexcept { ++destroyed; }
};
struct UnregisteredValue { int number = 0; };

namespace garden::ecs {
template<> struct ComponentType<WorldValue> {
    static constexpr ComponentKey key{"garden.tests.WorldValue", 1};
};
template<> struct ComponentType<UnregisteredValue> {
    static constexpr ComponentKey key{"garden.tests.UnregisteredWorldValue", 1};
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
}

int main() {
    auto& catalog = ComponentCatalog::instance();
    std::array<uint8_t, 32> digest{};
    digest[0] = 0xe1;
    const auto owner = ComponentOwnerId::module(digest, 1);
    garden::ecs::ComponentDescriptor descriptor{ComponentType<WorldValue>::key, owner, sizeof(WorldValue), alignof(WorldValue),
        componentCapabilities<WorldValue>(), makeComponentOperations<WorldValue>()};
    ComponentId id{};
    const auto registration = catalog.registerComponent(descriptor, id);
    check(registration == ComponentCatalogResult::Success, "register component metadata");
    if (registration != ComponentCatalogResult::Success) return 1;
    basicAndReuse();
    moves();
    teardown();
    check(WorldValue::constructed == WorldValue::destroyed, "all component values destroyed exactly once");
    check(catalog.unregisterOwner(owner) == ComponentCatalogResult::Success, "worlds release catalog leases");
    return failures ? 1 : 0;
}
