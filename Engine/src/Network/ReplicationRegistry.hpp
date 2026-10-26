#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>
#include "BitStream.hpp"

namespace Net {

// Declares how one replicated component's fields travel in the snapshot
// extension block (replication.md §2). field_id is a stable wire id.
struct ReplicatedFieldDesc
{
    const char* name = "";
    uint16_t field_id = 0;
    void (*write)(BitWriter&, const void* component) = nullptr;
    bool (*read)(BitReader&, void* component) = nullptr;
};

struct ReplicatedComponentInfo
{
    uint16_t component_id = 0;
    std::vector<ReplicatedFieldDesc> fields; // ascending field_id
};

// Process-wide registry of replicated components for ext-block validation.
// Registration is game/template code; the engine ships no components here.
class ReplicationRegistry
{
public:
    static ReplicationRegistry& get()
    {
        static ReplicationRegistry instance;
        return instance;
    }

    void registerComponent(uint16_t component_id, std::vector<ReplicatedFieldDesc> fields)
    {
        ReplicatedComponentInfo info;
        info.component_id = component_id;
        info.fields = std::move(fields);
        components[component_id] = std::move(info);
    }

    bool isRegistered(uint16_t component_id) const
    {
        return components.find(component_id) != components.end();
    }

    // Returns false when the component is not registered locally.
    bool getFieldCount(uint16_t component_id, uint16_t& out_count) const
    {
        auto it = components.find(component_id);
        if (it == components.end()) return false;
        out_count = static_cast<uint16_t>(it->second.fields.size());
        return true;
    }

    const ReplicatedComponentInfo* find(uint16_t component_id) const
    {
        auto it = components.find(component_id);
        return it == components.end() ? nullptr : &it->second;
    }

    void clear() { components.clear(); }

    // Diagnostics: ext entries rejected for unknown field_mask bits, and
    // reserved WEAPON_SELECT values masked on input decode.
    uint32_t ext_field_mask_reject_count = 0;
    uint32_t weapon_select_reserved_count = 0;

private:
    std::unordered_map<uint16_t, ReplicatedComponentInfo> components;
};

} // namespace Net
