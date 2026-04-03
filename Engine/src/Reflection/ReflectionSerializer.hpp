#pragma once

#include "ReflectionRegistry.hpp"
#include <entt/entt.hpp>
#include "json.hpp"

// Serialize/deserialize entities using reflected component metadata.
// Produces a component-based JSON format that can handle any registered component,
// including game DLL-defined ones.
class ReflectionSerializer
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
    static void deserializeEntity(
        entt::registry& registry,
        entt::entity entity,
        const nlohmann::json& entity_json,
        const ReflectionRegistry& reflection);

    // Deserialize all entities from a level JSON document
    static void deserializeLevel(
        entt::registry& registry,
        const nlohmann::json& level_json,
        const ReflectionRegistry& reflection);

    // Serialize a single component's properties to JSON
    static nlohmann::json serializeComponent(
        const ComponentDescriptor& desc,
        const void* component);

    // Deserialize a single component's properties from JSON
    static void deserializeComponent(
        const ComponentDescriptor& desc,
        void* component,
        const nlohmann::json& json);
};
