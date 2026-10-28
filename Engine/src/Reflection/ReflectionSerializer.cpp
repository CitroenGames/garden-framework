#include "ReflectionSerializer.hpp"
#include "ReflectionPropertyOps.hpp"
#include <memory>
#include <new>
#include <stdexcept>

using json = nlohmann::json;

namespace
{
bool hasOnlyKnownProperties(const ComponentDescriptor& desc, const json& component)
{
    if (!component.is_object())
        return false;

    for (const auto& [name, _] : component.items())
    {
        bool known = false;
        for (const auto& property : desc.properties)
        {
            if (property.name == name)
            {
                known = true;
                break;
            }
        }
        if (!known)
            return false;
    }
    return true;
}
} // namespace

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

namespace
{
ReflectionDeserializeResult failure(const std::string& message) { return { false, message }; }

struct StagedComponent
{
    const ComponentDescriptor* desc;
    entt::entity entity;
    const json* input;
    void* value = nullptr;
    bool constructed = false;
    bool added = false;
    StagedComponent(const ComponentDescriptor& d, entt::entity e, const json& j)
        : desc(&d), entity(e), input(&j) {}
    ~StagedComponent() {
        if (constructed) desc->destruct(value);
        if (value) ::operator delete(value, std::align_val_t(desc->alignment));
    }
    StagedComponent(const StagedComponent&) = delete;
};
ReflectionDeserializeResult stage(StagedComponent& staged)
{
    const auto& d = *staged.desc;
    if (!staged.input->is_object()) return failure("Component '" + d.name + "' must be an object");
    if (d.reject_unknown_properties && !hasOnlyKnownProperties(d, *staged.input))
        return failure("Unknown property in component '" + d.name + "'");
    if (!d.construct_default || !d.destruct || !d.alignment)
        return failure("Component '" + d.name + "' cannot be staged");
    staged.value = ::operator new(d.size, std::align_val_t(d.alignment));
    d.construct_default(staged.value);
    staged.constructed = true;
    for (const auto& prop : d.properties)
    {
        if (!staged.input->contains(prop.name)) continue;
        if (!prop.swap_value || !prop.mutable_data ||
            !ReflectionPropertyOps::deserializeProperty(prop, staged.value, (*staged.input)[prop.name]))
            return failure("Invalid property '" + d.name + "." + prop.name + "'");
    }
    return {};
}
void publish(StagedComponent& staged, void* destination) noexcept
{
    for (const auto& prop : staged.desc->properties)
        if (staged.input->contains(prop.name))
            prop.swap_value(prop.mutable_data(staged.value), prop.mutable_data(destination));
}
}

ReflectionDeserializeResult ReflectionSerializer::deserializeComponent(
    const ComponentDescriptor& desc, void* component, const json& j)
{
    if (!component) return failure("Null component '" + desc.name + "'");
    try {
        StagedComponent staged(desc, entt::null, j);
        auto result = stage(staged);
        if (!result) return result;
        publish(staged, component);
        return {};
    } catch (const std::exception& e) { return failure("Component '" + desc.name + "': " + e.what()); }
      catch (...) { return failure("Component '" + desc.name + "' threw an exception"); }
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

ReflectionDeserializeResult ReflectionSerializer::deserializeEntity(
    entt::registry& registry, entt::entity entity, const json& entity_json,
    const ReflectionRegistry& reflection)
{
    return deserializeEntities(registry, {{entity, entity_json}}, reflection);
}

ReflectionDeserializeResult ReflectionSerializer::deserializeEntities(
    entt::registry& registry, const std::vector<std::pair<entt::entity, json>>& entities,
    const ReflectionRegistry& reflection)
{
    std::vector<std::unique_ptr<StagedComponent>> staged;
    try {
        for (const auto& [entity, input] : entities)
        {
            if (!registry.valid(entity) || !input.is_object()) return failure("Invalid entity or entity payload");
            if (!input.contains("components")) continue;
            const auto& components = input["components"];
            if (!components.is_object()) return failure("Entity components must be an object");
            for (const auto& [name, value] : components.items())
            {
                const auto* desc = reflection.findByName(name.c_str());
                if (!desc) return failure("Unknown component '" + name + "'");
                if (!desc->has || !desc->add || !desc->remove || !desc->get)
                    return failure("Incomplete component descriptor '" + name + "'");
                staged.push_back(std::make_unique<StagedComponent>(*desc, entity, value));
                auto result = stage(*staged.back());
                if (!result) return result;
            }
        }
        // Construct all missing components before publishing any values.
        for (auto& item : staged)
        {
            if (!item->desc->has(registry, item->entity)) {
                item->added = true;
                item->desc->add(registry, item->entity);
            }
            if (!item->desc->get(registry, item->entity))
                throw std::runtime_error("component construction did not create a component");
        }
        for (auto& item : staged) publish(*item, item->desc->get(registry, item->entity));
        return {};
    } catch (const std::exception& e) {
        for (auto it = staged.rbegin(); it != staged.rend(); ++it)
            if ((*it)->added && (*it)->desc->has(registry, (*it)->entity)) (*it)->desc->remove(registry, (*it)->entity);
        return failure(std::string("Component deserialization failed: ") + e.what());
    } catch (...) {
        for (auto it = staged.rbegin(); it != staged.rend(); ++it)
            if ((*it)->added && (*it)->desc->has(registry, (*it)->entity)) (*it)->desc->remove(registry, (*it)->entity);
        return failure("Component deserialization threw an exception");
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

ReflectionDeserializeResult ReflectionSerializer::deserializeLevel(
    entt::registry& registry, const json& level_json, const ReflectionRegistry& reflection)
{
    if (!level_json.is_object() || !level_json.contains("entities") || !level_json["entities"].is_array())
        return failure("Level entities must be an array");
    std::vector<entt::entity> created;
    try {
        std::vector<std::pair<entt::entity, json>> inputs;
        created.reserve(level_json["entities"].size());
        inputs.reserve(level_json["entities"].size());
        for (const auto& input : level_json["entities"]) {
            auto entity = registry.create();
            created.push_back(entity);
            inputs.emplace_back(entity, input);
        }
        auto result = deserializeEntities(registry, inputs, reflection);
        if (result) return result;
        for (auto e : created) if (registry.valid(e)) registry.destroy(e);
        return result;
    } catch (...) {
        for (auto e : created) if (registry.valid(e)) registry.destroy(e);
        return failure("Level deserialization threw an exception");
    }
}
