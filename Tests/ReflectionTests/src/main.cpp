#include "Components/Components.hpp"
#include "Components/LocalPlayerInputComponent.hpp"
#include "Reflection/EngineReflection.hpp"
#include "Reflection/ReflectionPropertyOps.hpp"
#include "Reflection/ReflectionRegistry.hpp"
#include "Reflection/ReflectionSerializer.hpp"
#include "LevelManager.hpp"
#include "Components/HierarchyComponents.hpp"
#include "Components/IdentityComponent.hpp"
#include "ECS/Hierarchy.hpp"
#include "Components/AnimationComponent.hpp"
#include "Components/AudioSourceComponent.hpp"
#include "Components/IKComponent.hpp"
#include "GameFramework/GameFrameworkComponents.hpp"
#include <vector>

#include <cmath>
#include <entt/entt.hpp>
#include <filesystem>
#include <fstream>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <iostream>
#include <map>
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
            r.field<&TestComponent::speed>("speed").range(0.0f, 10.0f);
            r.field<&TestComponent::health>("health");
            r.field<&TestComponent::enabled>("enabled");
            r.field<&TestComponent::name>("name");
            r.field<&TestComponent::position>("position");
            r.field<&TestComponent::rotation>("rotation");
            r.field<&TestComponent::matrix>("matrix");
            r.field<&TestComponent::mode>("mode").enumValues(test_mode_names, 2);
            r.field<&TestComponent::target>("target").visible();
        }
    };

    struct VectorComponent
    {
        std::vector<float> floats;
        std::vector<int> ints;
        std::vector<bool> bools;
        std::vector<std::string> strings;
        std::vector<EntityRef> refs;
        std::vector<glm::vec3> points;
        float scalar = 0.0f;

        static void reflect(Reflector<VectorComponent>& r)
        {
            r.display("Vector Component").category("Tests");
            r.field<&VectorComponent::floats>("floats");
            r.field<&VectorComponent::ints>("ints");
            r.field<&VectorComponent::bools>("bools");
            r.field<&VectorComponent::strings>("strings");
            r.field<&VectorComponent::refs>("refs");
            r.field<&VectorComponent::points>("points");
            r.field<&VectorComponent::scalar>("scalar");
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
        if (entity_json["components"]["TestComponent"]["mode"] != "Active")
            return fail(name, "enum property was not serialized by stable name");

        entt::registry loaded_registry;
        entt::entity loaded = loaded_registry.create();
        entity_json["components"]["TestComponent"]["mode"] = 1; // legacy import path
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

    bool testWaterComponentReflectionAndObjectVectorJson()
    {
        const std::string name = "water component reflection and object vector json";

        ReflectionRegistry reflection;
        registerEngineReflection(reflection);

        if (!reflection.findByName("WaterComponent"))
            return fail(name, "WaterComponent was not registered");
        if (!reflection.findByName("WaterVolumeComponent"))
            return fail(name, "WaterVolumeComponent was not registered");

        entt::registry registry;
        entt::entity entity = registry.create();

        nlohmann::json entity_json = nlohmann::json::object();
        entity_json["components"]["WaterComponent"] = {
            {"enabled", true},
            {"affects_rendering", false},
            {"affects_swimming", true},
            {"half_extents", {{"x", 12.0f}, {"y", 3.0f}, {"z", 8.0f}}},
            {"surface_offset", 1.5f},
            {"swim_speed_scale", 0.6f}
        };
        entity_json["components"]["WaterVolumeComponent"] = {
            {"enabled", true},
            {"half_extents", {{"x", 6.0f}, {"y", 2.0f}, {"z", 4.0f}}}
        };

        ReflectionSerializer::deserializeEntity(registry, entity, entity_json, reflection);

        auto* water = registry.try_get<WaterComponent>(entity);
        auto* volume = registry.try_get<WaterVolumeComponent>(entity);
        if (!water || !volume)
            return fail(name, "water components were not deserialized");
        if (!approx(water->half_extents.x, 12.0f) || !approx(water->half_extents.y, 3.0f) ||
            !approx(water->half_extents.z, 8.0f))
            return fail(name, "WaterComponent object vector JSON was not parsed");
        if (!approx(volume->half_extents.x, 6.0f) || !approx(volume->half_extents.y, 2.0f) ||
            !approx(volume->half_extents.z, 4.0f))
            return fail(name, "WaterVolumeComponent object vector JSON was not parsed");
        if (water->affects_rendering || !water->affects_swimming || !approx(water->surface_offset, 1.5f))
            return fail(name, "WaterComponent scalar fields were not deserialized");

        vertex vertices[3]{};
        mesh water_mesh(vertices, 3);
        WaterComponent render_water;
        applyWaterComponentToMesh(render_water, water_mesh);
        if (!water_mesh.uses_material_ranges || water_mesh.material_ranges.empty())
            return fail(name, "WaterComponent did not create a material range");
        if (!water_mesh.material_ranges.front().isWater())
            return fail(name, "WaterComponent material range was not flagged as water");

        return pass(name);
    }

    bool testReflectedLevelJsonMigration()
    {
        const std::string name = "reflected level json migration";

        LevelData data;
        data.metadata.level_name = "Reflection Test";

        LevelEntity entity;
        entity.name = "Old Name";
        entity.type = EntityType::Static;
        entity.reflected_components = nlohmann::json::object();
        entity.reflected_components["TagComponent"]["name"] = "Reflected Entity";
        entity.reflected_components["TransformComponent"]["position"] = {1.0f, 2.0f, 3.0f};
        entity.reflected_components["TransformComponent"]["rotation"] = {4.0f, 5.0f, 6.0f};
        entity.reflected_components["TransformComponent"]["scale"] = {1.0f, 2.0f, 3.0f};
        entity.reflected_components["RigidBodyComponent"]["mass"] = 25.0f;
        entity.reflected_components["RigidBodyComponent"]["apply_gravity"] = false;
        entity.reflected_components["RigidBodyComponent"]["motion_type"] = "Kinematic";
        entity.reflected_components["PlayerComponent"]["speed"] = 9.0f;
        entity.reflected_components["PlayerComponent"]["jump_force"] = 12.0f;
        entity.reflected_components["PlayerComponent"]["mouse_sensitivity"] = 2.5f;
        data.entities.push_back(entity);

        const std::filesystem::path path =
            std::filesystem::current_path() / "build" / "reflection_level_roundtrip.json";

        LevelManager manager;
        if (!manager.saveLevelToJSON(path.string(), data))
            return fail(name, "failed to save reflected level json");

        std::ifstream saved(path);
        nlohmann::json raw;
        saved >> raw;
        saved.close();

        if (!raw["entities"][0].contains("components"))
            return fail(name, "saved level did not use reflected components block");
        if (raw["entities"][0].contains("transform"))
            return fail(name, "saved reflected level also wrote legacy transform block");

        LevelData loaded;
        if (!manager.loadLevelFromJSON(path.string(), loaded))
            return fail(name, "failed to load reflected level json");

        std::filesystem::remove(path);

        if (loaded.entities.size() != 1)
            return fail(name, "loaded level entity count mismatch");

        const auto& out = loaded.entities[0];
        bool ok = out.name == "Reflected Entity"
            && out.type == EntityType::Player
            && approx(out.position.y, 2.0f)
            && approx(out.scale.z, 3.0f)
            && out.has_rigidbody
            && approx(out.mass, 25.0f)
            && !out.apply_gravity
            && out.body_motion_type == "Kinematic"
            && approx(out.speed, 9.0f)
            && out.reflected_components.contains("PlayerComponent");

        if (!ok)
            return fail(name, "loaded reflected level did not rebuild legacy fallback fields");

        return pass(name);
    }
    bool testVectorRoundTrip()
    {
        const std::string name = "vector<T> round-trip (empty/scalars/strings/EntityRef/vec3)";
        ReflectionRegistry reflection;
        reflection.reflect<VectorComponent>("VectorComponent", "reflection_tests");
        const ComponentDescriptor* desc = reflection.findByName("VectorComponent");
        if (!desc) return fail(name, "VectorComponent not registered");
        // Descriptor shape
        int array_count = 0;
        for (const auto& prop : desc->properties)
        {
            if (prop.name == "floats" && prop.type == EPropertyType::Array && prop.element_type == EPropertyType::Float) ++array_count;
            if (prop.name == "ints" && prop.type == EPropertyType::Array && prop.element_type == EPropertyType::Int) ++array_count;
            if (prop.name == "bools" && prop.type == EPropertyType::Array && prop.element_type == EPropertyType::Bool) ++array_count;
            if (prop.name == "strings" && prop.type == EPropertyType::Array && prop.element_type == EPropertyType::String) ++array_count;
            if (prop.name == "refs" && prop.type == EPropertyType::Array && prop.element_type == EPropertyType::EntityRef) ++array_count;
            if (prop.name == "points" && prop.type == EPropertyType::Array && prop.element_type == EPropertyType::Vec3) ++array_count;
        }
        if (array_count != 6) return fail(name, "array element types not deduced");

        entt::registry reg;
        entt::entity e = reg.create();
        auto& c = reg.emplace<VectorComponent>(e);
        c.floats = {1.5f, -2.0f, 3.25f};
        c.ints = {7, -8, 42};
        c.bools = {true, false, true};
        c.strings = {"alpha", "", "gamma delta"};
        EntityRef r1; r1.guid = EntityGuid{0x1111, 0x2222};
        EntityRef r2; r2.guid = EntityGuid{0xAAAA, 0xBBBB};
        c.refs = {r1, EntityRef{}, r2};
        c.points = {glm::vec3(1,2,3), glm::vec3(-4,5.5f,6)};
        c.scalar = 9.5f;

        nlohmann::json ej = ReflectionSerializer::serializeEntity(reg, e, reflection);
        auto vc = ej["components"]["VectorComponent"];
        if (!vc["floats"].is_array() || vc["floats"].size() != 3) return fail(name, "floats not JSON array");
        if (vc["refs"][0] != guidToString(r1.guid)) return fail(name, "EntityRef element not GUID string");
        if (vc["refs"][1] != guidToString(EntityGuid{})) return fail(name, "null EntityRef element wrong");
        if (vc["points"][1][1] != 5.5f) return fail(name, "vec3 element wrong");

        entt::registry reg2;
        entt::entity e2 = reg2.create();
        ReflectionSerializer::deserializeEntity(reg2, e2, ej, reflection);
        if (!reg2.all_of<VectorComponent>(e2)) return fail(name, "vector component not added");
        const auto& out = reg2.get<VectorComponent>(e2);
        if (!ReflectionPropertyOps::componentPropertiesEqual(*desc, &c, &out)) return fail(name, "equality after round-trip failed");
        if (out.floats != c.floats || out.ints != c.ints || out.strings != c.strings || out.refs != c.refs) return fail(name, "field mismatch after round-trip");
        if (out.bools.size() != 3 || !out.bools[0] || out.bools[1] || !out.bools[2]) return fail(name, "bool vector mismatch");
        if (!approx(out.points[1].y, 5.5f) || !approx(out.scalar, 9.5f)) return fail(name, "vec3/scalar mismatch");

        // Copy (must deep-copy vectors, not memcpy the vector object)
        VectorComponent copied;
        ReflectionPropertyOps::copyComponentProperties(*desc, &c, &copied);
        if (!ReflectionPropertyOps::componentPropertiesEqual(*desc, &c, &copied)) return fail(name, "copy mismatch");
        copied.floats.push_back(99.0f);
        if (c.floats.size() != 3) return fail(name, "copy aliased source vector");

        // Equality detects difference
        VectorComponent diff = c; diff.strings[0] = "changed";
        if (ReflectionPropertyOps::componentPropertiesEqual(*desc, &c, &diff)) return fail(name, "equality missed string diff");

        // Empty vectors round-trip
        entt::registry reg3; entt::entity e3 = reg3.create(); reg3.emplace<VectorComponent>(e3);
        nlohmann::json ej3 = ReflectionSerializer::serializeEntity(reg3, e3, reflection);
        entt::registry reg4; entt::entity e4 = reg4.create();
        ReflectionSerializer::deserializeEntity(reg4, e4, ej3, reflection);
        const auto& out4 = reg4.get<VectorComponent>(e4);
        if (!out4.floats.empty() || !out4.refs.empty() || !out4.strings.empty()) return fail(name, "empty vector round-trip not empty");

        // Invalid element must not mutate existing value
        VectorComponent guard; guard.floats = {1.0f, 2.0f};
        const PropertyDescriptor* floats_prop = nullptr;
        for (const auto& prop : desc->properties) if (prop.name == "floats") floats_prop = &prop;
        if (!floats_prop) return fail(name, "floats prop missing");
        if (ReflectionPropertyOps::deserializeProperty(*floats_prop, &guard, nlohmann::json::array({1.0f, "bad"})))
            return fail(name, "invalid vector element reported success");
        if (guard.floats.size() != 2 || !approx(guard.floats[0], 1.0f)) return fail(name, "invalid vector mutated value");
        return pass(name);
    }

    bool testHierarchyChildrenSerialization()
    {
        const std::string name = "hierarchy Children serialization does not corrupt Parent";
        ReflectionRegistry reflection;
        registerEngineReflection(reflection);
        if (!reflection.findByName("ChildrenComponent")) return fail(name, "ChildrenComponent not reflected");
        if (reflection.findByName("WorldTransformComponent")) return fail(name, "WorldTransformComponent must stay unreflected");
        if (reflection.findByName("PresentationTransformComponent")) return fail(name, "PresentationTransformComponent must stay unreflected");
        const ComponentDescriptor* children_desc = reflection.findByName("ChildrenComponent");
        bool is_array_ref = false;
        for (const auto& prop : children_desc->properties)
            if (prop.name == "children" && prop.type == EPropertyType::Array && prop.element_type == EPropertyType::EntityRef) is_array_ref = true;
        if (!is_array_ref) return fail(name, "children is not Array<EntityRef>");

        EntityGuid parent_guid{0x1000, 0x1};
        EntityGuid child_guid{0x1000, 0x2};
        EntityGuid child2_guid{0x1000, 0x3};
        entt::registry reg;
        entt::entity parent = reg.create();
        entt::entity child = reg.create();
        entt::entity child2 = reg.create();
        reg.emplace<IdentityComponent>(parent).guid = parent_guid;
        reg.emplace<IdentityComponent>(child).guid = child_guid;
        reg.emplace<IdentityComponent>(child2).guid = child2_guid;
        reg.emplace<TransformComponent>(parent);
        reg.emplace<TransformComponent>(child);
        reg.emplace<TransformComponent>(child2);
        reg.emplace<ParentComponent>(child).parent = EntityRef{parent_guid};
        reg.emplace<ParentComponent>(child2).parent = EntityRef{parent_guid};
        auto& children = reg.emplace<ChildrenComponent>(parent);
        children.children = {EntityRef{child_guid}, EntityRef{child2_guid}};

        nlohmann::json level = ReflectionSerializer::serializeLevel(reg, reflection);
        // Parent serialized as single GUID string, Children as array of GUID strings
        bool saw_children = false, saw_parent = false;
        for (auto& ej : level["entities"])
        {
            if (!ej.contains("components")) continue;
            if (ej["components"].contains("ChildrenComponent"))
            {
                auto arr = ej["components"]["ChildrenComponent"]["children"];
                if (!arr.is_array() || arr.size() != 2 || arr[0] != guidToString(child_guid)) return fail(name, "serialized Children wrong");
                saw_children = true;
            }
            if (ej["components"].contains("ParentComponent"))
            {
                if (ej["components"]["ParentComponent"]["parent"] != guidToString(parent_guid)) return fail(name, "serialized Parent corrupted");
                saw_parent = true;
            }
        }
        if (!saw_children || !saw_parent) return fail(name, "hierarchy components missing in level JSON");

        // Tamper Children in the file: rebuild from Parent must restore it and keep Parent intact
        for (auto& ej : level["entities"])
            if (ej.contains("components") && ej["components"].contains("ChildrenComponent"))
                ej["components"]["ChildrenComponent"]["children"] = nlohmann::json::array();

        entt::registry loaded;
        ReflectionSerializer::deserializeLevel(loaded, level, reflection);
        auto find_by_guid = [&](EntityGuid g) -> entt::entity {
            for (auto [e, id] : loaded.view<IdentityComponent>().each()) if (id.guid == g) return e;
            return entt::null;
        };
        entt::entity lparent = find_by_guid(parent_guid);
        entt::entity lchild = find_by_guid(child_guid);
        entt::entity lchild2 = find_by_guid(child2_guid);
        if (lparent == entt::null || lchild == entt::null || lchild2 == entt::null) return fail(name, "loaded entities missing");
        // Tampered empty Children was loaded as-is, Parent untouched
        const auto* tampered = loaded.try_get<ChildrenComponent>(lparent);
        if (!tampered || !tampered->children.empty()) return fail(name, "tampered Children not loaded as empty");
        Hierarchy::rebuildChildren(loaded, {lchild, lchild2});
        const auto* lp = loaded.try_get<ParentComponent>(lchild);
        if (!lp || lp->parent.guid != parent_guid) return fail(name, "Parent corrupted after Children rebuild");
        const auto* lc = loaded.try_get<ChildrenComponent>(lparent);
        if (!lc || lc->children.size() != 2 || lc->children[0].guid != child_guid || lc->children[1].guid != child2_guid)
            return fail(name, "Children not rebuilt from Parent");
        if (Hierarchy::parentOf(loaded, lchild) != lparent) return fail(name, "parentOf broken after load+rebuild");
        return pass(name);
    }

    bool testEntityRefLevelSaveLoadSave()
    {
        const std::string name = "level save/load/save preserves entity GUID and hierarchy references";
        ReflectionRegistry reflection;
        registerEngineReflection(reflection);

        const EntityGuid parent_guid{0x1001, 0x10};
        const EntityGuid child_guid{0x1002, 0x20};
        const EntityGuid sibling_guid{0x1003, 0x30};

        entt::registry source;
        const entt::entity parent = source.create();
        const entt::entity child = source.create();
        const entt::entity sibling = source.create();
        source.emplace<IdentityComponent>(parent, IdentityComponent{parent_guid});
        source.emplace<IdentityComponent>(child, IdentityComponent{child_guid});
        source.emplace<IdentityComponent>(sibling, IdentityComponent{sibling_guid});
        source.emplace<ParentComponent>(child, ParentComponent{EntityRef{parent_guid}});
        source.emplace<ParentComponent>(sibling, ParentComponent{EntityRef{parent_guid}});
        source.emplace<ChildrenComponent>(parent,
            ChildrenComponent{{EntityRef{child_guid}, EntityRef{sibling_guid}}});

        const nlohmann::json first_save = ReflectionSerializer::serializeLevel(source, reflection);
        auto entitiesByGuid = [](const nlohmann::json& level,
                                 std::map<std::string, nlohmann::json>& entities) -> bool
        {
            if (!level.contains("entities") || !level["entities"].is_array())
                return false;
            for (const auto& entity_json : level["entities"])
            {
                if (!entity_json.contains("components") ||
                    !entity_json["components"].contains("IdentityComponent"))
                    return false;
                const auto& identity = entity_json["components"]["IdentityComponent"];
                if (!identity.contains("guid") || !identity["guid"].is_string())
                    return false;
                if (!entities.emplace(identity["guid"].get<std::string>(), entity_json).second)
                    return false; // duplicate stable identity
            }
            return true;
        };

        std::map<std::string, nlohmann::json> first_by_guid;
        if (!entitiesByGuid(first_save, first_by_guid) || first_by_guid.size() != 3)
            return fail(name, "first save is missing unique identity GUIDs");

        entt::registry loaded;
        ReflectionSerializer::deserializeLevel(loaded, first_save, reflection);
        const nlohmann::json second_save = ReflectionSerializer::serializeLevel(loaded, reflection);

        std::map<std::string, nlohmann::json> second_by_guid;
        if (!entitiesByGuid(second_save, second_by_guid) || second_by_guid.size() != first_by_guid.size())
            return fail(name, "second save is missing or duplicates entity GUIDs");

        const std::string parent_id = guidToString(parent_guid);
        const std::string child_id = guidToString(child_guid);
        const std::string sibling_id = guidToString(sibling_guid);
        for (const std::string& guid : {parent_id, child_id, sibling_id})
        {
            const auto first = first_by_guid.find(guid);
            const auto second = second_by_guid.find(guid);
            if (first == first_by_guid.end() || second == second_by_guid.end())
                return fail(name, "entity GUID changed across load and second save");
            if (first->second["components"]["IdentityComponent"]["guid"] !=
                second->second["components"]["IdentityComponent"]["guid"])
                return fail(name, "serialized identity GUID changed on second save");
        }

        const auto& first_children = first_by_guid.at(parent_id)["components"]["ChildrenComponent"]["children"];
        const auto& second_children = second_by_guid.at(parent_id)["components"]["ChildrenComponent"]["children"];
        const nlohmann::json expected_children = nlohmann::json::array({child_id, sibling_id});
        if (first_children != expected_children || second_children != expected_children)
            return fail(name, "parent ChildrenComponent GUID references changed across round-trip");

        for (const std::string& child_id_value : {child_id, sibling_id})
        {
            const auto& first_parent = first_by_guid.at(child_id_value)["components"]["ParentComponent"]["parent"];
            const auto& second_parent = second_by_guid.at(child_id_value)["components"]["ParentComponent"]["parent"];
            if (first_parent != parent_id || second_parent != parent_id || first_parent != second_parent)
                return fail(name, "child ParentComponent GUID reference changed across round-trip");
        }
        return pass(name);
    }

}

    bool testNewComponentRoundTrips()
    {
        const std::string name = "new component round trips (animation/audio/IK/gameframework/EntityRef)";
        ReflectionRegistry reflection;
        registerEngineReflection(reflection);
        for (const char* n : {"AnimationComponent", "AudioSourceComponent", "IKComponent",
                              "GameModeComponent", "GameStateComponent", "PlayerStateComponent", "PlayerControllerComponent"})
            if (!reflection.findByName(n)) return fail(name, std::string("missing registration: ") + n);
        if (reflection.count() != 35) return fail(name, "unexpected registered component count");
        // MeshComponent is sidecar-only (shared_ptr<mesh> GPU resource,
        // persisted via level/prefab sidecar path). LocalPlayerInputComponent
        // is transient runtime intent. Neither is reflected.
        if (reflection.findByName("MeshComponent")) return fail(name, "MeshComponent must stay sidecar-exempt");
        if (reflection.findByName("LocalPlayerInputComponent"))
            return fail(name, "LocalPlayerInputComponent must stay runtime-only");

        // Animation: authoring fields only; runtime state absent from JSON.
        {
            entt::registry reg; auto e = reg.create();
            auto& c = reg.emplace<AnimationComponent>(e);
            c.skeleton_path = "assets/hero.skel"; c.clip_names = {"idle", "run"};
            c.default_clip = "idle"; c.playback_speed = 1.5f; c.loop = false; c.auto_play = true;
            c.bone_matrices.assign(2, glm::mat4(2.0f));
            auto j = ReflectionSerializer::serializeEntity(reg, e, reflection);
            auto cj = j["components"]["AnimationComponent"];
            for (const char* k : {"skeleton", "clips", "blender", "bone_matrices"})
                if (cj.contains(k)) return fail(name, "animation runtime field serialized");
            entt::registry reg2; auto e2 = reg2.create();
            ReflectionSerializer::deserializeEntity(reg2, e2, j, reflection);
            const auto& o = reg2.get<AnimationComponent>(e2);
            if (o.skeleton_path != c.skeleton_path || o.clip_names != c.clip_names ||
                o.default_clip != c.default_clip || !approx(o.playback_speed, 1.5f) ||
                o.loop != false || o.auto_play != true || !o.bone_matrices.empty())
                return fail(name, "animation fields mismatch");
        }
        // Audio: clip_path + tuning; resolved id/handle omitted.
        {
            entt::registry reg; auto e = reg.create();
            auto& c = reg.emplace<AudioSourceComponent>(e);
            c.clip_path = "assets/jump.wav"; c.volume = 0.5f; c.pitch = 1.25f;
            c.min_distance = 2.0f; c.max_distance = 80.0f; c.spatial = false;
            c.loop = true; c.auto_play = true; c.group = AudioGroup::Music;
            c.clip = 42; c.active_handle = 99; c.started = true;
            auto j = ReflectionSerializer::serializeEntity(reg, e, reflection);
            auto cj = j["components"]["AudioSourceComponent"];
            for (const char* k : {"clip", "active_handle", "started"})
                if (cj.contains(k)) return fail(name, "audio runtime field serialized");
            if (cj["group"] != "Music") return fail(name, "audio group not serialized by name");
            entt::registry reg2; auto e2 = reg2.create();
            ReflectionSerializer::deserializeEntity(reg2, e2, j, reflection);
            const auto& o = reg2.get<AudioSourceComponent>(e2);
            if (o.clip_path != c.clip_path || !approx(o.volume, 0.5f) || !approx(o.pitch, 1.25f) ||
                o.group != AudioGroup::Music || o.clip != INVALID_AUDIO_CLIP ||
                o.active_handle != INVALID_SOUND || o.started)
                return fail(name, "audio fields mismatch");
        }
        // IK: enabled flag only.
        {
            entt::registry reg; auto e = reg.create();
            reg.emplace<IKComponent>(e).enabled = false;
            auto j = ReflectionSerializer::serializeEntity(reg, e, reflection);
            auto cj = j["components"]["IKComponent"];
            if (cj.size() != 1 || !cj.contains("enabled")) return fail(name, "IK serialized unexpected fields");
            entt::registry reg2; auto e2 = reg2.create();
            ReflectionSerializer::deserializeEntity(reg2, e2, j, reflection);
            if (reg2.get<IKComponent>(e2).enabled) return fail(name, "IK enabled mismatch");
        }
        // MeshComponent: exempt from reflection (sidecar-only, see above).
        // An entity carrying only MeshComponent serializes no MeshComponent
        // JSON; the mesh is restored by the level/prefab sidecar path.
        {
            entt::registry reg; auto e = reg.create();
            reg.emplace<MeshComponent>(e);
            auto j = ReflectionSerializer::serializeEntity(reg, e, reflection);
            if (j.contains("components") && j["components"].contains("MeshComponent"))
                return fail(name, "MeshComponent must not serialize via reflection");
        }
        // GameFramework components + EntityRef GUID migration.
        {
            const EntityGuid g1{0xAA, 0x11}, g2{0xBB, 0x22}, g3{0xCC, 0x33};
            entt::registry reg; auto e = reg.create();
            auto& gm = reg.emplace<GameFramework::GameModeComponent>(e);
            gm.map_name = "arena"; gm.options = "?game=tdm"; gm.delayed_start = true; gm.min_respawn_delay = 2.5f;
            auto& gs = reg.emplace<GameFramework::GameStateComponent>(e);
            gs.player_states = {EntityRef{g1}, EntityRef{g2}};
            gs.server_world_time_seconds = 123.5; gs.elapsed_time = 77;
            auto& ps = reg.emplace<GameFramework::PlayerStateComponent>(e);
            ps.player_id = 7; ps.player_name = "Hero"; ps.score = 12.5f; ps.pawn = EntityRef{g1}; ps.freecam = EntityRef{g3};
            auto& pc = reg.emplace<GameFramework::PlayerControllerComponent>(e);
            pc.player_id = 7; pc.player_state = EntityRef{g2}; pc.pawn = EntityRef{g1};
            pc.freecam = EntityRef{g3}; pc.start_spot = EntityRef{g1}; pc.portal = "gate"; pc.spectator = true;
            auto& pr = reg.emplace<PlayerRepresentationComponent>(e);
            pr.tracked_player = EntityRef{g1};
            auto& cc = reg.emplace<ConstraintComponent>(e);
            cc.target_entity = EntityRef{g2};

            auto j = ReflectionSerializer::serializeEntity(reg, e, reflection);
            if (!j["components"]["PlayerStateComponent"]["pawn"].is_string() ||
                j["components"]["PlayerStateComponent"]["pawn"] != guidToString(g1))
                return fail(name, "PlayerState pawn not a GUID string");
            if (j["components"]["GameStateComponent"]["player_states"][1] != guidToString(g2))
                return fail(name, "player_states not GUID strings");
            if (!j["components"]["PlayerRepresentationComponent"]["tracked_player"].is_string() ||
                !j["components"]["ConstraintComponent"]["target_entity"].is_string())
                return fail(name, "migrated EntityRef fields not GUID strings");

            entt::registry reg2; auto e2 = reg2.create();
            ReflectionSerializer::deserializeEntity(reg2, e2, j, reflection);
            const auto& ogm = reg2.get<GameFramework::GameModeComponent>(e2);
            const auto& ogs = reg2.get<GameFramework::GameStateComponent>(e2);
            const auto& ops = reg2.get<GameFramework::PlayerStateComponent>(e2);
            const auto& opc = reg2.get<GameFramework::PlayerControllerComponent>(e2);
            if (ogm.map_name != "arena" || ogm.options != "?game=tdm" || !ogm.delayed_start || !approx(ogm.min_respawn_delay, 2.5f))
                return fail(name, "GameMode fields mismatch");
            if (ogs.player_states.size() != 2 || ogs.player_states[0].guid != g1 ||
                !approx(static_cast<float>(ogs.server_world_time_seconds), 123.5f) || ogs.elapsed_time != 77)
                return fail(name, "GameState fields mismatch");
            if (ops.player_id != 7 || ops.player_name != "Hero" || ops.pawn.guid != g1 || ops.freecam.guid != g3)
                return fail(name, "PlayerState fields mismatch");
            if (opc.player_state.guid != g2 || opc.start_spot.guid != g1 || opc.portal != "gate" || !opc.spectator)
                return fail(name, "PlayerController fields mismatch");
            if (reg2.get<PlayerRepresentationComponent>(e2).tracked_player.guid != g1 ||
                reg2.get<ConstraintComponent>(e2).target_entity.guid != g2)
                return fail(name, "migrated EntityRef fields mismatch");
        }
        return pass(name);
    }

int main()
{
    bool ok = true;
    ok = testRegistryAndWidgetResolution() && ok;
    ok = testComponentCopy() && ok;
    ok = testEntitySerializationRoundTrip() && ok;
    ok = testInvalidJsonDoesNotMutate() && ok;
    ok = testWaterComponentReflectionAndObjectVectorJson() && ok;
    ok = testReflectedLevelJsonMigration() && ok;
    ok = testVectorRoundTrip() && ok;
    ok = testHierarchyChildrenSerialization() && ok;
    ok = testEntityRefLevelSaveLoadSave() && ok;
    ok = testNewComponentRoundTrips() && ok;
    return ok ? 0 : 1;
}
