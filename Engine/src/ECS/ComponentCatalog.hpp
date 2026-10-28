#pragma once

#include "EngineExport.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <span>

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

// Module identities come from the host's verified image and validated API/ABI
// token. EngineCore has a separate reserved identity, not a synthetic digest.
enum class ComponentOwnerKind : uint8_t { Invalid, EngineCore, Module };
struct ComponentOwnerId {
    ComponentOwnerKind kind = ComponentOwnerKind::Invalid;
    std::array<uint8_t, 32> imageSha256{};
    int32_t apiAbiVersion = 0;

    static constexpr ComponentOwnerId engineCore() noexcept {
        return {ComponentOwnerKind::EngineCore, {}, 0};
    }
    static constexpr ComponentOwnerId module(std::array<uint8_t, 32> digest, int32_t token) noexcept {
        return {ComponentOwnerKind::Module, digest, token};
    }
    friend bool operator==(const ComponentOwnerId&, const ComponentOwnerId&) = default;
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
    ComponentOwnerId ownerId;
    size_t size = 0;
    size_t alignment = 0;
    uint32_t capabilities = 0;
    ComponentOperations operations;
};

// Validate a complete descriptor before staging it for a batch.
ENGINE_API bool isValidComponentDescriptor(const ComponentDescriptor&) noexcept;

enum class ComponentCatalogResult {
    // DuplicateKey means a compatible borrower; registerComponent returns the
    // provider's existing ID and leaves the provider's operations unchanged.
    Success, InvalidDescriptor, DuplicateKey, Conflict, NotFound, StaleId,
    InUse, ResourceExhausted, InternalError
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
    static ComponentCatalog& instance() noexcept;
    ComponentCatalogResult registerComponent(const ComponentDescriptor&, ComponentId& outId) noexcept;
    // Host supplies the owner; descriptor owner fields are ignored. Same-batch
    // duplicate names are rejected. On failure every output ID is invalid.
    // Compatible existing providers retain their operations and IDs.
    ComponentCatalogResult registerBatch(ComponentOwnerId owner, std::span<const ComponentDescriptor> descriptors,
        std::span<ComponentId> outIds) noexcept;
    ComponentCatalogResult resolve(const ComponentDescriptor&, ComponentId& outId) const noexcept;
    // Atomically retire all of the owner's components, or none if any is pinned.
    ComponentCatalogResult unregisterOwner(ComponentOwnerId ownerId) noexcept;
    // The loader serializes generation attachment with these transitions.
    // beginRetirement blocks subsequent registrations before pins are checked.
    ComponentCatalogResult beginRetirement(ComponentOwnerId ownerId) noexcept;
    ComponentCatalogResult reopenOwner(ComponentOwnerId ownerId) noexcept;
    // On failure, acquire leaves an existing outLease unchanged.
    ComponentCatalogResult acquire(ComponentId, ComponentPinKind, ComponentLease& outLease) noexcept;

    ComponentCatalog(const ComponentCatalog&) = delete;
    ComponentCatalog& operator=(const ComponentCatalog&) = delete;

private:
    friend class ComponentLease;
    struct Impl;
    explicit ComponentCatalog(Impl* impl) noexcept;
    ~ComponentCatalog();
    void release(ComponentId, ComponentPinKind) noexcept;
    Impl* m_impl;
};

} // namespace garden::ecs
