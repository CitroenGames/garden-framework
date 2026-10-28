#pragma once

#include "EngineExport.h"
#include "ReflectionRegistry.hpp"
#include <entt/entt.hpp>
#include "json.hpp"
#include <string>

struct ReflectionDeserializeResult
{
    bool success = true;
    std::string error;
    explicit operator bool() const { return success; }
};

// Serialize/deserialize entities using reflected component metadata.
// Produces a component-based JSON format that can handle any registered component,
// including game DLL-defined ones.
class ENGINE_API ReflectionSerializer
{
public:
    // Serialize a single entity's reflected components to JSON
    static nlohmann::json serializeEntity(
        entt::registry& registry,
        entt::entity entity,
        const ReflectionRegistry& reflection);

    // Serialize all entities to a level JSON document
    static nlohmann::json serializeLevel(
        entt::registry& registry,
        const ReflectionRegistry& reflection);

    // Deserialize a single entity from JSON, adding components to an existing entity
    [[nodiscard]] static ReflectionDeserializeResult deserializeEntity(
        entt::registry& registry,
        entt::entity entity,
        const nlohmann::json& entity_json,
        const ReflectionRegistry& reflection);

    // Atomically apply a cohort of entity payloads.
    [[nodiscard]] static ReflectionDeserializeResult deserializeEntities(
        entt::registry& registry,
        const std::vector<std::pair<entt::entity, nlohmann::json>>& entities,
        const ReflectionRegistry& reflection);

    // Deserialize all entities from a level JSON document
    [[nodiscard]] static ReflectionDeserializeResult deserializeLevel(
        entt::registry& registry,
        const nlohmann::json& level_json,
        const ReflectionRegistry& reflection);

    // Serialize a single component's properties to JSON
    static nlohmann::json serializeComponent(
        const ComponentDescriptor& desc,
        const void* component);

    // Deserialize a single component's properties from JSON
    [[nodiscard]] static ReflectionDeserializeResult deserializeComponent(
        const ComponentDescriptor& desc,
        void* component,
        const nlohmann::json& json);
};
