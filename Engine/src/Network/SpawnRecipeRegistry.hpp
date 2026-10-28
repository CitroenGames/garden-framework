#pragma once

#include <cstdint>
#include <map>
#include <vector>

#include "EngineExport.h"
#include "NetworkProtocol.hpp"

#include <entt/entt.hpp>

namespace Net {

// Template modules own recipe ids and callbacks. EngineCore only validates the
// current wire envelope, creates the generic network identity, and sequences
// recipe construction with immutable initial replicated state.
struct SpawnRecipeInfo
{
    uint16_t archetype_id = 0;
    bool (*validateInitialState)(const SpawnEntityMessage& message,
                                 const std::vector<ReplicatedExtEntry>& entries) = nullptr;
    bool (*construct)(entt::registry& registry, entt::entity entity,
                      const SpawnEntityMessage& message) = nullptr;
};

class ENGINE_API SpawnRecipeRegistry
{
public:
    static SpawnRecipeRegistry& get();

    bool registerRecipe(SpawnRecipeInfo info);
    bool unregisterRecipe(uint16_t archetype_id);
    void clear();

    const SpawnRecipeInfo* find(uint16_t archetype_id) const;

    // Parses the exact current init_ext block. It rejects unknown registered
    // component ids, duplicate component records, malformed framing, and
    // descriptor-invalid entries before an entity is constructed.
    bool validateSpawn(const SpawnEntityMessage& message,
                       std::vector<ReplicatedExtEntry>& out_entries);

    uint32_t descriptor_reject_count = 0;
    uint32_t unknown_recipe_reject_count = 0;
    uint32_t malformed_initial_state_reject_count = 0;
    uint32_t duplicate_initial_component_reject_count = 0;

private:
    std::map<uint16_t, SpawnRecipeInfo> recipes;
};

} // namespace Net
