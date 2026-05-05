#include "Reflection/ReflectionPropertyOps.hpp"
#include "Reflection/ReflectionRegistry.hpp"
#include "Reflection/ReflectionSerializer.hpp"

#include <cmath>
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <iostream>
#include <string>
#include <type_traits>

namespace
{
    enum class TestMode : int
    {
        Idle = 0,
        Active,
        Count
    };

    const char* test_mode_names[] = { "Idle", "Active" };

    struct TestComponent
    {
        float speed = 1.0f;
        int health = 100;
        bool enabled = true;
        std::string name = "default";
        glm::vec3 position{1.0f, 2.0f, 3.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::mat4 matrix{1.0f};
        TestMode mode = TestMode::Idle;
        entt::entity target = entt::null;

        static void reflect(Reflector<TestComponent>& r)
        {
            r.display("Test Component").category("Tests");
            r.property("speed", &TestComponent::speed).range(0.0f, 10.0f);
            r.property("health", &TestComponent::health);
            r.property("enabled", &TestComponent::enabled);
            r.property("name", &TestComponent::name);
            r.property("position", &TestComponent::position);
            r.property("rotation", &TestComponent::rotation);
            r.property("matrix", &TestComponent::matrix);
            r.property("mode", &TestComponent::mode).enumValues(test_mode_names, 2);
            r.property("target", &TestComponent::target).visible();
        }
    };

    bool approx(float a, float b, float epsilon = 0.001f)
    {
        return std::abs(a - b) <= epsilon;
    }

    bool fail(const std::string& name, const std::string& reason)
    {
        std::cerr << "[FAIL] " << name << ": " << reason << std::endl;
        return false;
    }

    bool pass(const std::string& name)
    {
        std::cout << "[PASS] " << name << std::endl;
        return true;
    }

    bool testRegistryAndWidgetResolution()
    {
        const std::string name = "registry and widget resolution";

        ReflectionRegistry registry;
        registry.reflect<TestComponent>("TestComponent", "reflection_tests");

        const ComponentDescriptor* desc = registry.findByName("TestComponent");
        if (!desc)
            return fail(name, "component was not registered by name");
        if (registry.findByTypeId(entt::type_hash<TestComponent>::value()) != desc)
            return fail(name, "component was not registered by type id");
        if (desc->properties.size() != 9)
            return fail(name, "unexpected property count");

        const PropertyDescriptor* target = nullptr;
        const PropertyDescriptor* speed = nullptr;
        for (const auto& prop : desc->properties)
        {
            if (std::string(prop.name) == "target")
                target = &prop;
            if (std::string(prop.name) == "speed")
                speed = &prop;
        }

        if (!target || !speed)
            return fail(name, "expected properties were not reflected");
        if (ReflectionPropertyOps::resolveWidget(*target) != EPropertyWidget::ReadOnly)
            return fail(name, "visible property was not forced read-only");
        if (ReflectionPropertyOps::resolveWidget(*speed) != EPropertyWidget::DragFloat)
            return fail(name, "float property did not resolve to drag widget");

        registry.unregisterBySource("reflection_tests");
        if (registry.count() != 0)
            return fail(name, "unregisterBySource did not remove reflected type");

        return pass(name);
    }

    bool testComponentCopy()
    {
        const std::string name = "component property copy";

        ReflectionRegistry registry;
        registry.reflect<TestComponent>("TestComponent", "reflection_tests");
        const ComponentDescriptor* desc = registry.findByName("TestComponent");
        if (!desc)
            return fail(name, "component was not registered");

        TestComponent source;
        source.speed = 4.0f;
        source.health = 75;
        source.enabled = false;
        source.name = "copied";
        source.position = glm::vec3(7.0f, 8.0f, 9.0f);
        source.rotation = glm::quat(0.5f, 0.1f, 0.2f, 0.3f);
        source.matrix[2][1] = 42.0f;
        source.mode = TestMode::Active;
        source.target = static_cast<entt::entity>(123u);

        std::aligned_storage_t<sizeof(TestComponent), alignof(TestComponent)> storage;
        desc->construct_default(&storage);
        auto* copied = reinterpret_cast<TestComponent*>(&storage);
        ReflectionPropertyOps::copyComponentProperties(*desc, &source, copied);

        bool ok = approx(copied->speed, source.speed)
            && copied->health == source.health
            && copied->enabled == source.enabled
            && copied->name == source.name
            && approx(copied->position.z, source.position.z)
            && approx(copied->rotation.w, source.rotation.w)
            && approx(copied->matrix[2][1], source.matrix[2][1])
            && copied->mode == source.mode
            && copied->target == source.target;

        desc->destruct(&storage);

        if (!ok)
            return fail(name, "copied component does not match source");

        return pass(name);
    }

    bool testEntitySerializationRoundTrip()
    {
        const std::string name = "entity serialization round trip";

        ReflectionRegistry reflection;
        reflection.reflect<TestComponent>("TestComponent", "reflection_tests");

        entt::registry registry;
        entt::entity entity = registry.create();
        auto& component = registry.emplace<TestComponent>(entity);
        component.speed = 3.5f;
        component.health = 64;
        component.enabled = false;
        component.name = "serialized";
        component.position = glm::vec3(10.0f, 11.0f, 12.0f);
        component.rotation = glm::quat(0.25f, 0.5f, 0.75f, 1.0f);
        component.matrix[3][2] = 99.0f;
        component.mode = TestMode::Active;
        component.target = static_cast<entt::entity>(42u);

        nlohmann::json entity_json = ReflectionSerializer::serializeEntity(registry, entity, reflection);
        if (!entity_json["components"].contains("TestComponent"))
            return fail(name, "serialized entity is missing reflected component");

        entt::registry loaded_registry;
        entt::entity loaded = loaded_registry.create();
        ReflectionSerializer::deserializeEntity(loaded_registry, loaded, entity_json, reflection);

        if (!loaded_registry.all_of<TestComponent>(loaded))
            return fail(name, "deserialization did not add reflected component");

        const auto& out = loaded_registry.get<TestComponent>(loaded);
        bool ok = approx(out.speed, component.speed)
            && out.health == component.health
            && out.enabled == component.enabled
            && out.name == component.name
            && approx(out.position.x, component.position.x)
            && approx(out.rotation.z, component.rotation.z)
            && approx(out.matrix[3][2], component.matrix[3][2])
            && out.mode == component.mode
            && out.target == component.target;

        if (!ok)
            return fail(name, "deserialized component does not match source");

        return pass(name);
    }

    bool testInvalidJsonDoesNotMutate()
    {
        const std::string name = "invalid json does not mutate";

        ReflectionRegistry reflection;
        reflection.reflect<TestComponent>("TestComponent", "reflection_tests");
        const ComponentDescriptor* desc = reflection.findByName("TestComponent");
        if (!desc)
            return fail(name, "component was not registered");

        TestComponent component;
        component.position = glm::vec3(1.0f, 2.0f, 3.0f);

        const PropertyDescriptor* position = nullptr;
        for (const auto& prop : desc->properties)
        {
            if (std::string(prop.name) == "position")
            {
                position = &prop;
                break;
            }
        }

        if (!position)
            return fail(name, "position property was not found");

        bool changed = ReflectionPropertyOps::deserializeProperty(*position, &component, nlohmann::json::array({ 4.0f, "bad", 6.0f }));
        if (changed)
            return fail(name, "invalid vector JSON reported success");
        if (!approx(component.position.x, 1.0f) || !approx(component.position.y, 2.0f) || !approx(component.position.z, 3.0f))
            return fail(name, "invalid vector JSON partially mutated the component");

        return pass(name);
    }
}

int main()
{
    bool ok = true;
    ok = testRegistryAndWidgetResolution() && ok;
    ok = testComponentCopy() && ok;
    ok = testEntitySerializationRoundTrip() && ok;
    ok = testInvalidJsonDoesNotMutate() && ok;
    return ok ? 0 : 1;
}
