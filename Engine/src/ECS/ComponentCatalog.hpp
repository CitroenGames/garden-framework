#pragma once

#include "EngineExport.h"
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace garden::ecs {

// The name is a canonical namespaced UTF-8 identifier supplied by the owner.
struct ComponentKey {
    std::string_view name;
    uint32_t revision = 0;
    friend bool operator==(const ComponentKey&, const ComponentKey&) = default;
};

// Process-local only; never persist or transmit this value.
struct ComponentId {
    uint32_t slot = UINT32_MAX;
    uint32_t generation = 0;
    friend bool operator==(const ComponentId&, const ComponentId&) = default;
};

enum ComponentCapability : uint32_t {
    ComponentDefaultConstruct = 1u << 0,
    ComponentCopyConstruct = 1u << 1,
    ComponentSerialize = 1u << 2,
};

// Operations return false on failure and must not let exceptions cross the DLL boundary.
// serialize receives an owner-defined context, whose lifetime is controlled by the caller.
struct ComponentOperations {
    bool (*defaultConstruct)(void*) noexcept = nullptr;
    void (*destroy)(void*) noexcept = nullptr;
    bool (*copyConstruct)(void*, const void*) noexcept = nullptr;
    bool (*serialize)(const void*, void*) noexcept = nullptr;
    friend bool operator==(const ComponentOperations&, const ComponentOperations&) = default;
};

struct ComponentDescriptor {
    ComponentKey key;
    uint64_t ownerId = 0;
    size_t size = 0;
    size_t alignment = 0;
    uint32_t capabilities = 0;
    ComponentOperations operations;
};

enum class ComponentCatalogResult {
    // DuplicateKey means a compatible borrower; registerComponent returns the
    // provider's existing ID and leaves the provider's operations unchanged.
    Success, InvalidDescriptor, DuplicateKey, Conflict, NotFound, StaleId,
    InUse, ResourceExhausted
};

enum class ComponentPinKind { Operation, LiveInstance };
class ComponentCatalog;

// Keep a lease alive while using its descriptor or calling any operation. A live
// instance lease must outlive its value and its destruction operation.
class ENGINE_API ComponentLease {
public:
    ComponentLease() noexcept = default;
    ~ComponentLease();
    ComponentLease(const ComponentLease&) = delete;
    ComponentLease& operator=(const ComponentLease&) = delete;
    ComponentLease(ComponentLease&& other) noexcept;
    ComponentLease& operator=(ComponentLease&& other) noexcept;
    const ComponentDescriptor* descriptor() const noexcept { return m_descriptor; }
    void reset() noexcept;

private:
    friend class ComponentCatalog;
    ComponentCatalog* m_catalog = nullptr;
    const ComponentDescriptor* m_descriptor = nullptr;
    ComponentId m_id;
    ComponentPinKind m_kind = ComponentPinKind::Operation;
};

// The returned process catalog is owned by EngineCore. Registration and lookup
// are explicit; resolving compares metadata and ignores operation pointers.
// Compatible duplicate registrations borrow the first provider's ID and operations.
// The provider must keep its DLL loaded until unregisterOwner succeeds after all
// operation and live-instance leases, including borrowers' leases, are released.
class ENGINE_API ComponentCatalog {
public:
    // Deliberately process-lifetime so static leases can safely release at shutdown.
    static ComponentCatalog& instance();
    ComponentCatalogResult registerComponent(const ComponentDescriptor&, ComponentId& outId);
    ComponentCatalogResult resolve(const ComponentDescriptor&, ComponentId& outId) const;
    // Atomically retire all of the owner's components, or none if any is pinned.
    ComponentCatalogResult unregisterOwner(uint64_t ownerId);
    ComponentCatalogResult acquire(ComponentId, ComponentPinKind, ComponentLease& outLease);

    ComponentCatalog(const ComponentCatalog&) = delete;
    ComponentCatalog& operator=(const ComponentCatalog&) = delete;

private:
    friend class ComponentLease;
    ComponentCatalog();
    ~ComponentCatalog();
    void release(ComponentId, ComponentPinKind) noexcept;
    struct Impl;
    Impl* m_impl;
};

} // namespace garden::ecs
