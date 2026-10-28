#include "ECS/ComponentCatalog.hpp"

#include <iostream>
#include <new>
#include <string>
#include <utility>

using namespace garden::ecs;
namespace {
int failures = 0;
void check(bool condition, const char* name) {
    std::cout << (condition ? "[PASS] " : "[FAIL] ") << name << '\n';
    if (!condition) ++failures;
}
struct Value { int number; };
int destroyed = 0;
bool construct(void* p) noexcept { new (p) Value{42}; return true; }
bool alternateConstruct(void* p) noexcept { new (p) Value{99}; return true; }
void destroy(void* p) noexcept { static_cast<Value*>(p)->~Value(); ++destroyed; }
bool copy(void* p, const void* source) noexcept {
    new (p) Value{static_cast<const Value*>(source)->number}; return true;
}
bool serialize(const void* p, void* context) noexcept {
    *static_cast<int*>(context) = static_cast<const Value*>(p)->number; return true;
}
ComponentDescriptor descriptor() {
    return {{"garden.tests.CatalogValue", 3}, 0x4267, sizeof(Value), alignof(Value),
        ComponentDefaultConstruct | ComponentCopyConstruct | ComponentSerialize,
        {construct, destroy, copy, serialize}};
}
}
int main() {
    auto& catalog = ComponentCatalog::instance();
    auto d = descriptor();
    ComponentId id{};
    check(catalog.registerComponent(d, id) == ComponentCatalogResult::Success &&
        id.slot != UINT32_MAX && id.generation != 0, "register explicit name and revision");

    ComponentId found{};
    auto consumer = d;
    consumer.operations = {};
    check(catalog.resolve(consumer, found) == ComponentCatalogResult::Success && found == id,
        "consumer metadata resolves without owner thunks");
    auto otherRevision = consumer;
    ++otherRevision.key.revision;
    check(catalog.resolve(otherRevision, found) == ComponentCatalogResult::Conflict,
        "revision participates in stable key");
    auto missing = consumer;
    missing.key.name = "garden.tests.Missing";
    check(catalog.resolve(missing, found) == ComponentCatalogResult::NotFound,
        "unknown explicit name is not found");
    auto borrower = d;
    borrower.operations.defaultConstruct = alternateConstruct;
    check(catalog.registerComponent(borrower, found) == ComponentCatalogResult::DuplicateKey && found == id,
        "compatible borrower gets provider id despite different thunk");
    auto conflict = d;
    ++conflict.key.revision;
    check(catalog.registerComponent(conflict, found) == ComponentCatalogResult::Conflict,
        "conflicting revision registration rejected");
    conflict = d;
    ++conflict.ownerId;
    check(catalog.resolve(conflict, found) == ComponentCatalogResult::Conflict &&
        catalog.registerComponent(conflict, found) == ComponentCatalogResult::Conflict,
        "different owner conflicts on resolve and registration");
    auto sibling = d;
    sibling.key.name = "garden.tests.CatalogSibling";
    ComponentId siblingId{};
    check(catalog.registerComponent(sibling, siblingId) == ComponentCatalogResult::Success,
        "register second type for same owner");

    auto invalid = d;
    invalid.operations.destroy = nullptr;
    check(catalog.registerComponent(invalid, found) == ComponentCatalogResult::InvalidDescriptor,
        "missing destroy operation rejected");
    invalid = d;
    invalid.operations.copyConstruct = nullptr;
    check(catalog.registerComponent(invalid, found) == ComponentCatalogResult::InvalidDescriptor,
        "missing capability operation rejected");
    invalid = d;
    invalid.alignment = 3;
    check(catalog.registerComponent(invalid, found) == ComponentCatalogResult::InvalidDescriptor,
        "non power of two alignment rejected");
    invalid = d;
    invalid.size = 3;
    invalid.alignment = 2;
    check(catalog.registerComponent(invalid, found) == ComponentCatalogResult::InvalidDescriptor,
        "size inconsistent with alignment rejected");
    invalid = d;
    invalid.key.revision = 0;
    check(catalog.resolve(invalid, found) == ComponentCatalogResult::InvalidDescriptor,
        "zero revision rejected");
    invalid = d;
    invalid.key.name = "unnamespaced";
    check(catalog.registerComponent(invalid, found) == ComponentCatalogResult::InvalidDescriptor,
        "unnamespaced key rejected");
    invalid = d;
    const std::string malformedName = std::string("garden.tests.bad") + std::string("\xC0\xAF", 2);
    invalid.key.name = malformedName;
    check(catalog.registerComponent(invalid, found) == ComponentCatalogResult::InvalidDescriptor,
        "malformed UTF-8 key rejected");
    invalid = d;
    invalid.capabilities |= 1u << 31;
    check(catalog.registerComponent(invalid, found) == ComponentCatalogResult::InvalidDescriptor,
        "unknown capability bits rejected");
    check(catalog.unregisterOwner(d.ownerId + 1) == ComponentCatalogResult::NotFound,
        "unknown owner cannot retire registrations");

    ComponentLease operation;
    auto invalidPin = static_cast<ComponentPinKind>(99);
    ComponentLease rejectedPin;
    check(catalog.acquire(id, invalidPin, rejectedPin) == ComponentCatalogResult::InvalidDescriptor,
        "unknown pin kind rejected");
    check(catalog.acquire(id, ComponentPinKind::Operation, operation) == ComponentCatalogResult::Success,
        "acquire operation lease");
    check(catalog.unregisterOwner(d.ownerId) == ComponentCatalogResult::InUse,
        "operation lease blocks owner retirement");
    check(catalog.resolve(sibling, found) == ComponentCatalogResult::Success && found == siblingId &&
        catalog.resolve(consumer, found) == ComponentCatalogResult::Success && found == id,
        "operation pin leaves both owner types registered");
    ComponentLease moved(std::move(operation));
    check(operation.descriptor() == nullptr && moved.descriptor() != nullptr,
        "move construction transfers lease");
    ComponentLease assigned;
    assigned = std::move(moved);
    check(moved.descriptor() == nullptr && assigned.descriptor() != nullptr,
        "move assignment transfers lease");
    assigned.reset();
    assigned.reset();
    check(assigned.descriptor() == nullptr, "reset releases lease and is idempotent");

    ComponentLease instance;
    check(catalog.acquire(id, ComponentPinKind::LiveInstance, instance) == ComponentCatalogResult::Success,
        "acquire live instance lease");
    check(catalog.unregisterOwner(d.ownerId) == ComponentCatalogResult::InUse,
        "live instance lease independently blocks owner retirement");
    check(catalog.resolve(sibling, found) == ComponentCatalogResult::Success && found == siblingId &&
        catalog.resolve(consumer, found) == ComponentCatalogResult::Success && found == id,
        "live instance pin leaves both owner types registered");
    alignas(Value) unsigned char first[sizeof(Value)];
    alignas(Value) unsigned char second[sizeof(Value)];
    const auto* ops = instance.descriptor() ? &instance.descriptor()->operations : nullptr;
    check(ops && ops->defaultConstruct == construct,
        "first provider callback remains authoritative");
    bool constructed = ops && ops->defaultConstruct(first);
    bool copied = constructed && ops->copyConstruct(second, first);
    int serialized = 0;
    bool serializedOk = copied && ops->serialize(second, &serialized);
    check(constructed && copied && serializedOk && serialized == 42,
        "owner callbacks construct copy and serialize");
    if (copied) ops->destroy(second);
    if (constructed) ops->destroy(first);
    check(destroyed == 2, "owner destroy callback runs for both values");
    std::string ownerName(instance.descriptor()->key.name);
    bool grew = true;
    for (int i = 0; i < 32; ++i) {
        auto extra = d;
        const std::string extraName = "garden.tests.Growth" + std::to_string(i);
        extra.key.name = extraName;
        ComponentId extraId{};
        grew &= catalog.registerComponent(extra, extraId) == ComponentCatalogResult::Success;
    }
    check(grew, "register additional component descriptors");
    check(instance.descriptor() && instance.descriptor()->key.name == ownerName,
        "leased descriptor name survives catalog growth");
    instance.reset();
    check(catalog.unregisterOwner(d.ownerId) == ComponentCatalogResult::Success,
        "unregister after all leases released");
    check(catalog.acquire(id, ComponentPinKind::Operation, operation) == ComponentCatalogResult::StaleId &&
        catalog.acquire(siblingId, ComponentPinKind::Operation, operation) == ComponentCatalogResult::StaleId &&
        catalog.unregisterOwner(d.ownerId) == ComponentCatalogResult::NotFound,
        "owner retirement makes both ids stale");
    check(catalog.resolve(consumer, found) == ComponentCatalogResult::NotFound,
        "unregistered key no longer resolves");
    ComponentId replacement{};
    check(catalog.registerComponent(d, replacement) == ComponentCatalogResult::Success &&
        replacement.slot == id.slot && replacement.generation != id.generation,
        "re-registration advances slot generation");
    check(catalog.acquire(id, ComponentPinKind::LiveInstance, operation) == ComponentCatalogResult::StaleId,
        "old generation remains stale after re-registration");
    check(catalog.unregisterOwner(d.ownerId) == ComponentCatalogResult::Success,
        "cleanup replacement registration");
    return failures ? 1 : 0;
}
