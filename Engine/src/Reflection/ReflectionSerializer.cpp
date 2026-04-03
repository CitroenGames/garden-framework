#include "ReflectionSerializer.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>
#include <cstring>

using json = nlohmann::json;

// ---- Property serialization helpers ----

static json serializeProperty(const PropertyDescriptor& prop, const void* component)
{
    const char* ptr = static_cast<const char*>(component) + prop.offset;

    switch (prop.type)
    {
    case EPropertyType::Float:
        return *reinterpret_cast<const float*>(ptr);

    case EPropertyType::Int:
        return *reinterpret_cast<const int*>(ptr);

    case EPropertyType::Bool:
        return *reinterpret_cast<const bool*>(ptr);

    case EPropertyType::String:
        return *reinterpret_cast<const std::string*>(ptr);

    case EPropertyType::Vec2:
    {
        auto& v = *reinterpret_cast<const glm::vec2*>(ptr);
        return json::array({v.x, v.y});
    }
    case EPropertyType::Vec3:
    {
        auto& v = *reinterpret_cast<const glm::vec3*>(ptr);
        return json::array({v.x, v.y, v.z});
    }
    case EPropertyType::Vec4:
    {
        auto& v = *reinterpret_cast<const glm::vec4*>(ptr);
        return json::array({v.x, v.y, v.z, v.w});
    }
    case EPropertyType::Quat:
    {
        auto& q = *reinterpret_cast<const glm::quat*>(ptr);
        return json::array({q.x, q.y, q.z, q.w});
    }
    case EPropertyType::Enum:
        return *reinterpret_cast<const int*>(ptr);

    case EPropertyType::Entity:
        // Serialize entity as integer ID (limited, but safe)
        return static_cast<uint32_t>(*reinterpret_cast<const entt::entity*>(ptr));

    default:
        return nullptr;
    }
}

static void deserializeProperty(const PropertyDescriptor& prop, void* component, const json& value)
{
    char* ptr = static_cast<char*>(component) + prop.offset;

    switch (prop.type)
    {
    case EPropertyType::Float:
        if (value.is_number())
            *reinterpret_cast<float*>(ptr) = value.get<float>();
        break;

    case EPropertyType::Int:
        if (value.is_number_integer())
            *reinterpret_cast<int*>(ptr) = value.get<int>();
        break;

    case EPropertyType::Bool:
        if (value.is_boolean())
            *reinterpret_cast<bool*>(ptr) = value.get<bool>();
        break;

    case EPropertyType::String:
        if (value.is_string())
            *reinterpret_cast<std::string*>(ptr) = value.get<std::string>();
        break;

    case EPropertyType::Vec2:
        if (value.is_array() && value.size() >= 2)
        {
            auto& v = *reinterpret_cast<glm::vec2*>(ptr);
            v.x = value[0].get<float>();
            v.y = value[1].get<float>();
        }
        break;

    case EPropertyType::Vec3:
        if (value.is_array() && value.size() >= 3)
        {
            auto& v = *reinterpret_cast<glm::vec3*>(ptr);
            v.x = value[0].get<float>();
            v.y = value[1].get<float>();
            v.z = value[2].get<float>();
        }
        break;

    case EPropertyType::Vec4:
        if (value.is_array() && value.size() >= 4)
        {
            auto& v = *reinterpret_cast<glm::vec4*>(ptr);
            v.x = value[0].get<float>();
            v.y = value[1].get<float>();
            v.z = value[2].get<float>();
            v.w = value[3].get<float>();
        }
        break;

    case EPropertyType::Quat:
        if (value.is_array() && value.size() >= 4)
        {
            auto& q = *reinterpret_cast<glm::quat*>(ptr);
            q.x = value[0].get<float>();
            q.y = value[1].get<float>();
            q.z = value[2].get<float>();
            q.w = value[3].get<float>();
        }
        break;

    case EPropertyType::Enum:
        if (value.is_number_integer())
            *reinterpret_cast<int*>(ptr) = value.get<int>();
        break;

    case EPropertyType::Entity:
        if (value.is_number_unsigned())
            *reinterpret_cast<entt::entity*>(ptr) =
                static_cast<entt::entity>(value.get<uint32_t>());
        break;

    default:
        break;
    }
}

// ---- Component-level ----

json ReflectionSerializer::serializeComponent(
    const ComponentDescriptor& desc,
    const void* component)
{
    json j = json::object();
    for (uint32_t i = 0; i < desc.property_count; i++)
    {
        const PropertyDescriptor& prop = desc.properties[i];
        j[prop.name] = serializeProperty(prop, component);
    }
    return j;
}

void ReflectionSerializer::deserializeComponent(
    const ComponentDescriptor& desc,
    void* component,
    const json& j)
{
    for (uint32_t i = 0; i < desc.property_count; i++)
    {
        const PropertyDescriptor& prop = desc.properties[i];
        if (j.contains(prop.name))
            deserializeProperty(prop, component, j[prop.name]);
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

    auto view = registry.view<entt::entity>();
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
