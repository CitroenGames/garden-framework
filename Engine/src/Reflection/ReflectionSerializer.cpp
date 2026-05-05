#include "ReflectionSerializer.hpp"
#include "ReflectionPropertyOps.hpp"

using json = nlohmann::json;

// ---- Component-level ----

json ReflectionSerializer::serializeComponent(
    const ComponentDescriptor& desc,
    const void* component)
{
    json j = json::object();
    for (const auto& prop : desc.properties)
        j[prop.name] = ReflectionPropertyOps::serializeProperty(prop, component);
    return j;
}

void ReflectionSerializer::deserializeComponent(
    const ComponentDescriptor& desc,
    void* component,
    const json& j)
{
    for (const auto& prop : desc.properties)
    {
        if (j.contains(prop.name))
            ReflectionPropertyOps::deserializeProperty(prop, component, j[prop.name]);
        // Missing fields keep their default values
    }
}

// ---- Entity-level ----

json ReflectionSerializer::serializeEntity(
    entt::registry& registry,
    entt::entity entity,
    const ReflectionRegistry& reflection)
{
    json entity_json = json::object();
    json components = json::object();

    for (auto& desc : reflection.getAll())
    {
        if (desc.has(registry, entity))
        {
            void* comp = desc.get(registry, entity);
            if (comp)
                components[desc.name] = serializeComponent(desc, comp);
        }
    }

    entity_json["components"] = components;
    return entity_json;
}

void ReflectionSerializer::deserializeEntity(
    entt::registry& registry,
    entt::entity entity,
    const json& entity_json,
    const ReflectionRegistry& reflection)
{
    if (!entity_json.contains("components"))
        return;

    const json& components = entity_json["components"];
    for (auto& [comp_name, comp_json] : components.items())
    {
        const ComponentDescriptor* desc = reflection.findByName(comp_name.c_str());
        if (!desc)
        {
            fprintf(stderr, "[ReflectionSerializer] Unknown component '%s', skipping\n",
                    comp_name.c_str());
            continue;
        }

        // Add component if not present
        if (!desc->has(registry, entity))
            desc->add(registry, entity);

        // Get pointer and deserialize
        void* comp = desc->get(registry, entity);
        if (comp)
            deserializeComponent(*desc, comp, comp_json);
    }
}

// ---- Level-level ----

json ReflectionSerializer::serializeLevel(
    entt::registry& registry,
    const ReflectionRegistry& reflection)
{
    json level = json::object();
    json entities = json::array();

    for (auto entity : registry.view<entt::entity>())
    {
        entities.push_back(serializeEntity(registry, entity, reflection));
    }

    level["format"] = "garden_reflected";
    level["version"] = 1;
    level["entities"] = entities;
    return level;
}

void ReflectionSerializer::deserializeLevel(
    entt::registry& registry,
    const json& level_json,
    const ReflectionRegistry& reflection)
{
    if (!level_json.contains("entities"))
        return;

    for (auto& entity_json : level_json["entities"])
    {
        entt::entity entity = registry.create();
        deserializeEntity(registry, entity, entity_json, reflection);
    }
}
