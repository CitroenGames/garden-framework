#include "SpawnRecipeRegistry.hpp"

#include "BitStream.hpp"
#include "ReplicationRegistry.hpp"

#include <limits>
#include <set>
#include <utility>

namespace Net {
namespace {

bool hasCurrentRecipeContract(const SpawnRecipeInfo& info)
{
    return info.archetype_id != 0 && info.validateInitialState != nullptr &&
           info.construct != nullptr;
}

bool decodeCurrentInitialState(const std::vector<uint8_t>& bytes,
                               std::vector<ReplicatedExtEntry>& out_entries,
                               SpawnRecipeRegistry& diagnostics)
{
    out_entries.clear();
    // The v3 SPAWN_ENTITY envelope expressly permits an empty init block.
    if (bytes.empty()) {
        return true;
    }
    if (bytes.size() > (std::numeric_limits<uint16_t>::max)()) {
        ++diagnostics.malformed_initial_state_reject_count;
        return false;
    }

    BitReader reader(bytes.data(), bytes.size());
    if (!reader.canRead(16)) {
        ++diagnostics.malformed_initial_state_reject_count;
        return false;
    }

    const uint16_t component_count = reader.readUInt16();
    std::set<uint16_t> seen_component_ids;
    out_entries.reserve(component_count);
    for (uint16_t index = 0; index < component_count; ++index) {
        if (!reader.canRead(48)) {
            ++diagnostics.malformed_initial_state_reject_count;
            out_entries.clear();
            return false;
        }
        const uint16_t component_id = reader.readUInt16();
        const uint16_t field_mask = reader.readUInt16();
        const uint16_t payload_size = reader.readUInt16();
        if (component_id == 0 || !seen_component_ids.insert(component_id).second) {
            ++diagnostics.duplicate_initial_component_reject_count;
            out_entries.clear();
            return false;
        }
        if (!reader.canRead(static_cast<size_t>(payload_size) * 8u)) {
            ++diagnostics.malformed_initial_state_reject_count;
            out_entries.clear();
            return false;
        }

        std::vector<uint8_t> payload;
        payload.reserve(payload_size);
        for (uint16_t payload_index = 0; payload_index < payload_size; ++payload_index) {
            payload.push_back(reader.readByte());
        }
        if (reader.hasError() || !ReplicationRegistry::get().isRegistered(component_id)) {
            ++diagnostics.malformed_initial_state_reject_count;
            out_entries.clear();
            return false;
        }
        out_entries.push_back({component_id, field_mask, std::move(payload)});
    }

    if (reader.hasError() || reader.getBitPosition() != bytes.size() * 8u ||
        !ReplicationRegistry::get().validateEcsEntries(out_entries)) {
        ++diagnostics.malformed_initial_state_reject_count;
        out_entries.clear();
        return false;
    }
    return true;
}

} // namespace

SpawnRecipeRegistry& SpawnRecipeRegistry::get()
{
    static SpawnRecipeRegistry instance;
    return instance;
}

bool SpawnRecipeRegistry::registerRecipe(SpawnRecipeInfo info)
{
    if (!hasCurrentRecipeContract(info) || recipes.contains(info.archetype_id)) {
        ++descriptor_reject_count;
        return false;
    }
    recipes.emplace(info.archetype_id, std::move(info));
    return true;
}

bool SpawnRecipeRegistry::unregisterRecipe(uint16_t archetype_id)
{
    return recipes.erase(archetype_id) != 0;
}

void SpawnRecipeRegistry::clear()
{
    recipes.clear();
    descriptor_reject_count = 0;
    unknown_recipe_reject_count = 0;
    malformed_initial_state_reject_count = 0;
    duplicate_initial_component_reject_count = 0;
}

const SpawnRecipeInfo* SpawnRecipeRegistry::find(uint16_t archetype_id) const
{
    const auto it = recipes.find(archetype_id);
    return it == recipes.end() ? nullptr : &it->second;
}

bool SpawnRecipeRegistry::validateSpawn(const SpawnEntityMessage& message,
                                        std::vector<ReplicatedExtEntry>& out_entries)
{
    const SpawnRecipeInfo* recipe = find(message.archetype_id);
    if (message.network_id == 0 || recipe == nullptr) {
        ++unknown_recipe_reject_count;
        out_entries.clear();
        return false;
    }
    if (!decodeCurrentInitialState(message.init_ext, out_entries, *this)) {
        return false;
    }
    if (!recipe->validateInitialState(message, out_entries)) {
        ++malformed_initial_state_reject_count;
        out_entries.clear();
        return false;
    }
    return true;
}

} // namespace Net
