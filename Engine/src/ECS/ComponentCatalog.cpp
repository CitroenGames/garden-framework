#include "ECS/ComponentCatalog.hpp"

#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

namespace garden::ecs {
namespace {

// Reject malformed UTF-8, overlong sequences, surrogates and non-scalar values.
bool validName(std::string_view name) noexcept {
    if (name.empty() || name.front() == '.' || name.back() == '.') return false;
    bool hasSeparator = false;
    bool previousSeparator = false;
    for (size_t i = 0; i < name.size();) {
        const auto c = static_cast<unsigned char>(name[i]);
        if (c < 0x80) {
            if (c <= 0x20 || c == 0x7f || (c == '.' && previousSeparator)) return false;
            previousSeparator = c == '.';
            hasSeparator |= previousSeparator;
            ++i;
            continue;
        }
        previousSeparator = false;
        unsigned count = 0;
        uint32_t value = 0;
        uint32_t minimum = 0;
        if (c >= 0xc2 && c <= 0xdf) { count = 2; value = c & 0x1f; minimum = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { count = 3; value = c & 0x0f; minimum = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { count = 4; value = c & 0x07; minimum = 0x10000; }
        else return false;
        if (name.size() - i < count) return false;
        for (unsigned j = 1; j < count; ++j) {
            const auto next = static_cast<unsigned char>(name[i + j]);
            if ((next & 0xc0) != 0x80) return false;
            value = (value << 6) | (next & 0x3f);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
        i += count;
    }
    return hasSeparator;
}

bool validMetadata(const ComponentDescriptor& d) noexcept {
    constexpr uint32_t known = ComponentDefaultConstruct | ComponentCopyConstruct | ComponentSerialize;
    return validName(d.key.name) && d.key.revision != 0 && d.ownerId != 0 && d.size != 0 &&
        d.alignment != 0 && (d.alignment & (d.alignment - 1)) == 0 && d.size % d.alignment == 0 &&
        (d.capabilities & ~known) == 0;
}

bool validDescriptor(const ComponentDescriptor& d) noexcept {
    return validMetadata(d) && d.operations.destroy &&
        bool(d.operations.defaultConstruct) == bool(d.capabilities & ComponentDefaultConstruct) &&
        bool(d.operations.copyConstruct) == bool(d.capabilities & ComponentCopyConstruct) &&
        bool(d.operations.serialize) == bool(d.capabilities & ComponentSerialize);
}

bool sameMetadata(const ComponentDescriptor& a, const ComponentDescriptor& b) noexcept {
    return a.key == b.key && a.ownerId == b.ownerId && a.size == b.size &&
        a.alignment == b.alignment && a.capabilities == b.capabilities;
}
}

struct ComponentCatalog::Impl {
    struct Entry {
        std::string name;
        ComponentDescriptor descriptor;
        uint32_t generation = 1;
        uint64_t operations = 0;
        uint64_t instances = 0;
        bool active = false;
    };
    mutable std::mutex mutex;
    std::vector<std::unique_ptr<Entry>> entries;
};

ComponentCatalog::ComponentCatalog(Impl* impl) noexcept : m_impl(impl) {}
ComponentCatalog::~ComponentCatalog() = default;

ComponentCatalog& ComponentCatalog::instance() noexcept {
    // Static storage avoids a fallible singleton allocation. Intentionally never
    // destroy these objects: leases in other static destructors can still release.
    alignas(Impl) static unsigned char implStorage[sizeof(Impl)];
    alignas(ComponentCatalog) static unsigned char catalogStorage[sizeof(ComponentCatalog)];
    static ComponentCatalog* const catalog = []() noexcept {
        auto* impl = ::new (implStorage) Impl;
        return ::new (catalogStorage) ComponentCatalog(impl);
    }();
    return *catalog;
}

ComponentCatalogResult ComponentCatalog::registerComponent(const ComponentDescriptor& d, ComponentId& outId) noexcept {
    outId = {};
    if (!validDescriptor(d)) return ComponentCatalogResult::InvalidDescriptor;
    try {
        std::lock_guard lock(m_impl->mutex);
        size_t freeSlot = m_impl->entries.size();
        for (size_t i = 0; i < m_impl->entries.size(); ++i) {
            const auto& e = *m_impl->entries[i];
            if (e.active && e.name == d.key.name) {
                if (!sameMetadata(e.descriptor, d)) return ComponentCatalogResult::Conflict;
                outId = {static_cast<uint32_t>(i), e.generation};
                return ComponentCatalogResult::DuplicateKey;
            }
            if (!e.active && e.generation != UINT32_MAX && freeSlot == m_impl->entries.size()) freeSlot = i;
        }
        if (freeSlot >= UINT32_MAX) return ComponentCatalogResult::ResourceExhausted;
        // Complete all fallible work before changing an existing slot or publishing
        // a new one. The final swap and descriptor assignments cannot throw.
        std::string name(d.key.name);
        if (freeSlot == m_impl->entries.size()) {
            auto entry = std::make_unique<Impl::Entry>();
            entry->name.swap(name);
            entry->descriptor = d;
            entry->descriptor.key.name = entry->name;
            entry->active = true;
            m_impl->entries.push_back(std::move(entry));
        } else {
            auto& entry = *m_impl->entries[freeSlot];
            entry.name.swap(name);
            entry.descriptor = d;
            entry.descriptor.key.name = entry.name;
            entry.active = true;
        }
        outId = {static_cast<uint32_t>(freeSlot), m_impl->entries[freeSlot]->generation};
        return ComponentCatalogResult::Success;
    } catch (const std::bad_alloc&) {
        return ComponentCatalogResult::ResourceExhausted;
    } catch (...) {
        return ComponentCatalogResult::InternalError;
    }
}

ComponentCatalogResult ComponentCatalog::resolve(const ComponentDescriptor& d, ComponentId& outId) const noexcept {
    outId = {};
    if (!validMetadata(d)) return ComponentCatalogResult::InvalidDescriptor;
    try {
        std::lock_guard lock(m_impl->mutex);
        for (size_t i = 0; i < m_impl->entries.size(); ++i) {
            const auto& e = *m_impl->entries[i];
            if (e.active && e.name == d.key.name) {
                if (!sameMetadata(e.descriptor, d)) return ComponentCatalogResult::Conflict;
                outId = {static_cast<uint32_t>(i), e.generation};
                return ComponentCatalogResult::Success;
            }
        }
        return ComponentCatalogResult::NotFound;
    } catch (const std::bad_alloc&) {
        return ComponentCatalogResult::ResourceExhausted;
    } catch (...) {
        return ComponentCatalogResult::InternalError;
    }
}

ComponentCatalogResult ComponentCatalog::unregisterOwner(uint64_t ownerId) noexcept {
    try {
        std::lock_guard lock(m_impl->mutex);
        bool found = false;
        for (const auto& entry : m_impl->entries) {
            const auto& e = *entry;
            if (!e.active || e.descriptor.ownerId != ownerId) continue;
            found = true;
            if (e.operations || e.instances) return ComponentCatalogResult::InUse;
        }
        if (!found) return ComponentCatalogResult::NotFound;
        for (auto& entry : m_impl->entries) {
            auto& e = *entry;
            if (!e.active || e.descriptor.ownerId != ownerId) continue;
            e.active = false;
            e.descriptor = {};
            e.name.clear();
            if (e.generation != UINT32_MAX) ++e.generation;
        }
        return ComponentCatalogResult::Success;
    } catch (const std::bad_alloc&) {
        return ComponentCatalogResult::ResourceExhausted;
    } catch (...) {
        return ComponentCatalogResult::InternalError;
    }
}

ComponentCatalogResult ComponentCatalog::acquire(ComponentId id, ComponentPinKind kind, ComponentLease& outLease) noexcept {
    if (kind != ComponentPinKind::Operation && kind != ComponentPinKind::LiveInstance)
        return ComponentCatalogResult::InvalidDescriptor;
    try {
        std::lock_guard lock(m_impl->mutex);
        if (id.slot >= m_impl->entries.size()) return ComponentCatalogResult::StaleId;
        auto& e = *m_impl->entries[id.slot];
        if (!e.active || e.generation != id.generation) return ComponentCatalogResult::StaleId;
        auto& count = kind == ComponentPinKind::Operation ? e.operations : e.instances;
        if (count == std::numeric_limits<uint64_t>::max()) return ComponentCatalogResult::ResourceExhausted;
        ++count;
        if (outLease.m_catalog) {
            // All leases come from the one process catalog; update both counts
            // while holding the same lock, after the replacement is validated.
            auto& previous = *m_impl->entries[outLease.m_id.slot];
            auto& previousCount = outLease.m_kind == ComponentPinKind::Operation
                ? previous.operations : previous.instances;
            --previousCount;
        }
        outLease.m_catalog = this;
        outLease.m_descriptor = &e.descriptor;
        outLease.m_id = id;
        outLease.m_kind = kind;
        return ComponentCatalogResult::Success;
    } catch (const std::bad_alloc&) {
        return ComponentCatalogResult::ResourceExhausted;
    } catch (...) {
        return ComponentCatalogResult::InternalError;
    }
}

void ComponentCatalog::release(ComponentId id, ComponentPinKind kind) noexcept {
    try {
        std::lock_guard lock(m_impl->mutex);
        auto& e = *m_impl->entries[id.slot];
        auto& count = kind == ComponentPinKind::Operation ? e.operations : e.instances;
        --count;
    } catch (...) {
        // Preserve the pin if locking fails; retirement must remain blocked.
    }
}

ComponentLease::~ComponentLease() { reset(); }
ComponentLease::ComponentLease(ComponentLease&& other) noexcept
    : m_catalog(other.m_catalog), m_descriptor(other.m_descriptor), m_id(other.m_id), m_kind(other.m_kind) {
    other.m_catalog = nullptr;
    other.m_descriptor = nullptr;
}
ComponentLease& ComponentLease::operator=(ComponentLease&& other) noexcept {
    if (this != &other) {
        reset();
        m_catalog = other.m_catalog;
        m_descriptor = other.m_descriptor;
        m_id = other.m_id;
        m_kind = other.m_kind;
        other.m_catalog = nullptr;
        other.m_descriptor = nullptr;
    }
    return *this;
}
void ComponentLease::reset() noexcept {
    if (m_catalog) m_catalog->release(m_id, m_kind);
    m_catalog = nullptr;
    m_descriptor = nullptr;
}

} // namespace garden::ecs
