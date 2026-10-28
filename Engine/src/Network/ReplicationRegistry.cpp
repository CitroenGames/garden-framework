#include "ReplicationRegistry.hpp"
#include "NetworkProtocol.hpp"

#include <algorithm>
#include <utility>

namespace Net {
namespace {

bool hasCurrentFieldLayout(const ReplicatedComponentInfo& info)
{
    if (info.component_id == 0 || info.fields.size() > 16 ||
        info.extract == nullptr || info.validate == nullptr || info.apply == nullptr) {
        return false;
    }

    for (uint16_t index = 0; index < info.fields.size(); ++index) {
        if (info.fields[index].name == nullptr || info.fields[index].field_id != index) {
            return false;
        }
    }
    return true;
}

bool hasCurrentEntryContract(const ReplicatedComponentInfo& info,
                             const ReplicatedExtEntry& entry)
{
    if (entry.component_id != info.component_id || entry.field_mask == 0) {
        return false;
    }
    const uint16_t field_count = static_cast<uint16_t>(info.fields.size());
    return field_count == 16 || (entry.field_mask >> field_count) == 0;
}

} // namespace

ReplicationRegistry& ReplicationRegistry::get()
{
    static ReplicationRegistry instance;
    return instance;
}

bool ReplicationRegistry::registerComponent(ReplicatedComponentInfo info)
{
    if (!hasCurrentFieldLayout(info) || components.contains(info.component_id)) {
        ++descriptor_reject_count;
        return false;
    }
    components.emplace(info.component_id, std::move(info));
    return true;
}

bool ReplicationRegistry::unregisterComponent(uint16_t component_id)
{
    return components.erase(component_id) != 0;
}

void ReplicationRegistry::clear()
{
    components.clear();
    descriptor_reject_count = 0;
    ext_field_mask_reject_count = 0;
    ext_ecs_apply_reject_count = 0;
    weapon_select_reserved_count = 0;
}

bool ReplicationRegistry::isRegistered(uint16_t component_id) const
{
    return components.contains(component_id);
}

bool ReplicationRegistry::getFieldCount(uint16_t component_id, uint16_t& out_count) const
{
    const auto it = components.find(component_id);
    if (it == components.end()) return false;
    out_count = static_cast<uint16_t>(it->second.fields.size());
    return true;
}

const ReplicatedComponentInfo* ReplicationRegistry::find(uint16_t component_id) const
{
    const auto it = components.find(component_id);
    return it == components.end() ? nullptr : &it->second;
}

bool ReplicationRegistry::extractEntity(const entt::registry& registry, entt::entity entity,
                                        std::vector<ReplicatedExtEntry>& out_entries) const
{
    out_entries.clear();
    for (const auto& [component_id, info] : components) {
        ReplicatedExtEntry entry;
        if (!info.extract(registry, entity, entry)) {
            continue;
        }
        if (!hasCurrentEntryContract(info, entry) || !info.validate(entry)) {
            out_entries.clear();
            return false;
        }
        out_entries.push_back(std::move(entry));
    }
    return true;
}

bool ReplicationRegistry::validateEcsEntries(const std::vector<ReplicatedExtEntry>& entries) const
{
    for (const ReplicatedExtEntry& entry : entries) {
        const ReplicatedComponentInfo* info = find(entry.component_id);
        if (info == nullptr || !hasCurrentEntryContract(*info, entry) || !info->validate(entry)) {
            return false;
        }
    }
    return true;
}

bool ReplicationRegistry::applyEcsEntries(entt::registry& registry, entt::entity entity,
                                          const std::vector<ReplicatedExtEntry>& entries)
{
    if (!validateEcsEntries(entries)) {
        ++ext_ecs_apply_reject_count;
        return false;
    }
    for (const ReplicatedExtEntry& entry : entries) {
        find(entry.component_id)->apply(registry, entity, entry);
    }
    return true;
}

} // namespace Net
