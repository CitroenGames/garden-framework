#pragma once

#include "EngineExport.h"
#include "BitStream.hpp"

#include <cstdint>
#include <map>
#include <vector>

#include <entt/entt.hpp>

namespace Net {

struct ReplicatedExtEntry;

// Declares the current wire fields of one replicated ECS component. Field ids
// are stable protocol ids and must be contiguous from zero so field_mask has
// one unambiguous meaning.
struct ReplicatedFieldDesc
{
    const char* name = "";
    uint16_t field_id = 0;
};

// Template-owned callbacks bridge opaque extension bytes to an ECS component.
// extract returns false when the entity does not carry this component. validate
// must not mutate ECS. apply is called only after every entry validates and is
// therefore an infallible commit operation.
struct ReplicatedComponentInfo
{
    uint16_t component_id = 0;
    std::vector<ReplicatedFieldDesc> fields; // ascending contiguous field_id
    bool (*extract)(const entt::registry&, entt::entity, ReplicatedExtEntry&) = nullptr;
    bool (*validate)(const ReplicatedExtEntry&) = nullptr;
    void (*apply)(entt::registry&, entt::entity, const ReplicatedExtEntry&) = nullptr;
};

// EngineCore-owned registry of the template components carried by HAS_EXT.
// Unknown component ids are skipped by the length-framed serializer. A known
// entry whose mask exceeds the current descriptor rejects that entity's whole
// extension block. Entries with missing/mismatched ECS callbacks reject before
// any ECS mutation. Callers must unregister template descriptors before their
// DLL unloads so no callback pointer can survive module lifetime.
class ENGINE_API ReplicationRegistry
{
public:
    static ReplicationRegistry& get();

    bool registerComponent(ReplicatedComponentInfo info);
    bool unregisterComponent(uint16_t component_id);
    void clear();

    bool isRegistered(uint16_t component_id) const;
    bool getFieldCount(uint16_t component_id, uint16_t& out_count) const;
    const ReplicatedComponentInfo* find(uint16_t component_id) const;

    // Extracts all registered ECS components in ascending stable component id.
    // An invalid extractor result is rejected and no partial list is returned.
    bool extractEntity(const entt::registry& registry, entt::entity entity,
                       std::vector<ReplicatedExtEntry>& out_entries) const;

    // Validates every known entry without ECS mutation. Serializer-level
    // unknown-component skipping has already happened before this boundary.
    bool validateEcsEntries(const std::vector<ReplicatedExtEntry>& entries) const;

    // Atomic at the validation boundary: all entries validate before any apply
    // callback runs. apply callbacks are required to be infallible commits.
    bool applyEcsEntries(entt::registry& registry, entt::entity entity,
                         const std::vector<ReplicatedExtEntry>& entries);

    // Diagnostics: malformed current descriptors, unknown field masks, and
    // ECS bridge rejects. Unknown component entries remain skipped by framing.
    uint32_t descriptor_reject_count = 0;
    uint32_t ext_field_mask_reject_count = 0;
    uint32_t ext_ecs_apply_reject_count = 0;
    uint32_t weapon_select_reserved_count = 0;

private:
    std::map<uint16_t, ReplicatedComponentInfo> components;
};

} // namespace Net
