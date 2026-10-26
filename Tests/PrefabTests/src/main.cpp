#include "Components/Components.hpp"
#include "Components/HierarchyComponents.hpp"
#include "Components/IdentityComponent.hpp"
#include "Components/PrefabInstanceComponent.hpp"
#include "ECS/EntityLifecycle.hpp"
#include "ECS/Hierarchy.hpp"
#include "Events/EventBus.hpp"
#include "Prefab/PrefabManager.hpp"
#include "Reflection/EngineReflection.hpp"
#include "Reflection/ReflectionRegistry.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
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
bool near(float a, float b, float eps = 1e-4f) { return std::abs(a - b) <= eps; }
bool nearVec(const glm::vec3& a, const glm::vec3& b, float eps = 1e-4f)
{
    return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps);
}

// Test-only reflected component exercising EntityRef + vector<EntityRef>
// remapping through prefab instantiation.
struct PrefabLinkComponent
{
    EntityRef target;
    std::vector<EntityRef> links;
    float value = 0.0f;

    static void reflect(Reflector<PrefabLinkComponent>& r)
    {
        r.display("Prefab Link").category("Tests");
        r.field<&PrefabLinkComponent::target>("target").widget(EPropertyWidget::EntityRef);
        r.field<&PrefabLinkComponent::links>("links");
        r.field<&PrefabLinkComponent::value>("value");
    }
};

ReflectionRegistry g_reflection;
fs::path g_dir;

std::string prefabPath(const std::string& file)
{
    return (g_dir / file).string();
}

void writeFile(const std::string& path, const std::string& contents)
{
    std::ofstream f(path);
    f << contents;
}

size_t entityCount(entt::registry& reg)
{
    size_t n = 0;
    for (auto e : reg.view<entt::entity>())
    {
        (void)e;
        ++n;
    }
    return n;
}

EntityGuid guidOf(entt::registry& reg, entt::entity e)
{
    auto* id = reg.try_get<IdentityComponent>(e);
    return id ? id->guid : EntityGuid{};
}

bool findNodeGuid(const PrefabData& data, const std::string& node_name, EntityGuid& out)
{
    for (const auto& node : data.json["nodes"])
        if (node.contains("name") && node["name"] == node_name &&
            node.contains("guid") && guidFromString(node["guid"].get<std::string>(), out))
            return true;
    return false;
}

// Builds Root(10,0,0) -> Child(1,0,0) -> Grand(0,2,0) in `reg`.
// Child links: target=Grand (internal), links=[Root (internal), External].
struct AuthoredTree
{
    entt::entity root, child, grand, external;
    EntityGuid root_guid, child_guid, grand_guid, external_guid;
};

AuthoredTree authorTree(entt::registry& reg, EntityLifecycle& life)
{
    AuthoredTree t;
    t.root = life.create("Root");
    t.child = life.create("Child");
    t.grand = life.create("Grand");
    t.external = life.create("External");
    auto setPos = [&](entt::entity e, glm::vec3 p) {
        auto& tr = reg.emplace<TransformComponent>(e);
        tr.position = p;
        tr.rotation = glm::vec3(0.0f);
        tr.scale = glm::vec3(1.0f);
    };
    setPos(t.root, {10, 0, 0});
    setPos(t.child, {1, 0, 0});
    setPos(t.grand, {0, 2, 0});
    setPos(t.external, {99, 99, 99});
    Hierarchy::attach(reg, t.child, t.root);
    Hierarchy::attach(reg, t.grand, t.child);
    auto& link = reg.emplace<PrefabLinkComponent>(t.child);
    link.target = EntityRef{guidOf(reg, t.grand)};
    link.links = {EntityRef{guidOf(reg, t.root)}, EntityRef{guidOf(reg, t.external)}};
    link.value = 3.5f;
    t.root_guid = guidOf(reg, t.root);
    t.child_guid = guidOf(reg, t.child);
    t.grand_guid = guidOf(reg, t.grand);
    t.external_guid = guidOf(reg, t.external);
    return t;
}

bool saveTreePrefab(const std::string& file)
{
    entt::registry reg;
    EntityLifecycle life(reg, 42);
    AuthoredTree t = authorTree(reg, life);
    return PrefabManager::get().savePrefab(reg, t.root, prefabPath(file));
}

// V2 core: subtree save -> instantiate twice -> distinct GUIDs, internal
// refs inside own instance, external preserved, hierarchy transforms.
bool testSubtreeInstantiateTwice()
{
    const std::string name = "Prefab v2 subtree instantiate twice";
    EventBus::get().clear();
    entt::registry author;
    EntityLifecycle author_life(author, 42);
    AuthoredTree t = authorTree(author, author_life);
    const std::string path = prefabPath("tree.prefab");
    if (!PrefabManager::get().savePrefab(author, t.root, path))
        return fail(name, "savePrefab failed");

    PrefabData data;
    if (!PrefabManager::loadPrefab(path, data))
        return fail(name, "loadPrefab failed");
    if (!data.valid || data.version != 1 && data.json["version"] != 2)
        return fail(name, "not normalized to v2");
    if (data.json["nodes"].size() != 3)
        return fail(name, "expected 3 nodes");
    if (data.root_guid != t.root_guid)
        return fail(name, "file root guid != authoring root guid");

    entt::registry reg;
    EntityLifecycle life(reg, 7);
    PrefabInstantiateResult a = PrefabManager::get().instantiate(reg, data);
    PrefabInstantiateResult b = PrefabManager::get().instantiate(reg, data);
    if (!a.success || !b.success)
        return fail(name, "instantiate failed");
    if (a.nodes.size() != 3 || b.nodes.size() != 3)
        return fail(name, "expected 3 entities per instance");

    entt::entity a_child = a.nodes[t.child_guid];
    entt::entity a_grand = a.nodes[t.grand_guid];
    entt::entity b_child = b.nodes[t.child_guid];
    entt::entity b_grand = b.nodes[t.grand_guid];

    // Fresh, distinct GUIDs per instance and vs the authoring entities.
    if (guidOf(reg, a.root) == t.root_guid || guidOf(reg, b.root) == t.root_guid ||
        guidOf(reg, a.root) == guidOf(reg, b.root) ||
        guidOf(reg, a_grand) == guidOf(reg, b_grand))
        return fail(name, "instance GUIDs not fresh/distinct");

    // Hierarchy rebuilt: Parent links + Children mirrors.
    if (Hierarchy::parentOf(reg, a_child) != a.root ||
        Hierarchy::parentOf(reg, a_grand) != a_child)
        return fail(name, "parent links wrong in instance A");
    auto* children = reg.try_get<ChildrenComponent>(a.root);
    if (!children || children->children.size() != 1 ||
        children->children[0].guid != guidOf(reg, a_child))
        return fail(name, "children mirror wrong in instance A");

    // Internal refs remapped into own instance; external preserved.
    const auto& la = reg.get<PrefabLinkComponent>(a_child);
    if (la.target.guid != guidOf(reg, a_grand))
        return fail(name, "A internal target not remapped into A");
    if (la.links.size() != 2 || la.links[0].guid != guidOf(reg, a.root))
        return fail(name, "A internal link not remapped into A");
    if (la.links[1].guid != t.external_guid)
        return fail(name, "A external link not preserved");
    const auto& lb = reg.get<PrefabLinkComponent>(b_child);
    if (lb.target.guid != guidOf(reg, b_grand) || lb.target.guid == la.target.guid)
        return fail(name, "B internal target leaked across instances");
    if (!near(la.value, 3.5f))
        return fail(name, "scalar property lost");

    // Instance linkage stamped on every node, shared per instance.
    const auto& pa = reg.get<PrefabInstanceComponent>(a_grand);
    const auto& pb = reg.get<PrefabInstanceComponent>(b_grand);
    if (pa.source_node_guid != t.grand_guid || pa.source_root_guid != t.root_guid)
        return fail(name, "source node/root guid wrong");
    if (pa.instance_guid.isNull() || pa.instance_guid == pb.instance_guid)
        return fail(name, "instance guid not distinct per instance");
    if (reg.get<PrefabInstanceComponent>(a.root).instance_guid != pa.instance_guid)
        return fail(name, "instance guid not shared across nodes");

    // Hierarchy transforms: locals compose into world.
    Hierarchy::updateTransformPropagation(reg);
    if (!nearVec(reg.get<WorldTransformComponent>(a.root).position, {10, 0, 0}))
        return fail(name, "A root world wrong");
    if (!nearVec(reg.get<WorldTransformComponent>(a_child).position, {11, 0, 0}))
        return fail(name, "A child world wrong");
    if (!nearVec(reg.get<WorldTransformComponent>(a_grand).position, {11, 2, 0}))
        return fail(name, "A grandchild world wrong");
    return pass(name);
}

// Overrides hit only the targeted node; compute/revert round-trip.
bool testOverrides()
{
    const std::string name = "Prefab v2 overrides targeted + compute/revert";
    EventBus::get().clear();
    if (!saveTreePrefab("tree2.prefab"))
        return fail(name, "save failed");
    PrefabData data;
    if (!PrefabManager::loadPrefab(prefabPath("tree2.prefab"), data))
        return fail(name, "load failed");
    EntityGuid child_src;
    if (!findNodeGuid(data, "Child", child_src))
        return fail(name, "child node not found in file");

    entt::registry reg;
    EntityLifecycle life(reg, 11);
    PrefabInstantiateOptions opts;
    opts.overrides.push_back(PropertyOverride{child_src, "TransformComponent", "position",
                                              nlohmann::json::array({9.0, 9.0, 9.0})});
    PrefabInstantiateResult r = PrefabManager::get().instantiate(reg, data, opts);
    if (!r.success)
        return fail(name, "instantiate failed");
    entt::entity child = r.nodes[child_src];
    if (!nearVec(reg.get<TransformComponent>(child).position, {9, 9, 9}))
        return fail(name, "override not applied to child");
    if (!nearVec(reg.get<TransformComponent>(r.root).position, {10, 0, 0}))
        return fail(name, "override leaked to root");

    // Mutate locally, compute the diff, then revert to source values.
    reg.get<TransformComponent>(r.root).position = {5, 5, 5};
    auto diffs = PrefabManager::get().computeOverrides(reg, r.root);
    bool saw_root_pos = false;
    for (const auto& d : diffs)
        if (d.component == "TransformComponent" && d.property == "position" &&
            d.node == data.root_guid)
            saw_root_pos = true;
    if (!saw_root_pos)
        return fail(name, "computeOverrides missed mutated root position");
    if (!PrefabManager::get().revertOverrides(reg, r.root))
        return fail(name, "revertOverrides failed");
    if (!nearVec(reg.get<TransformComponent>(r.root).position, {10, 0, 0}))
        return fail(name, "revert did not restore root position");
    // The instantiation-time override was caller-supplied, so revert
    // restores the child's source position too.
    if (!nearVec(reg.get<TransformComponent>(child).position, {1, 0, 0}))
        return fail(name, "revert did not restore child position");
    return pass(name);
}

// spawnAt applies the spawn transform to the root only.
bool testSpawnAtRootOnly()
{
    const std::string name = "Prefab v2 spawnAt root-only transform";
    EventBus::get().clear();
    if (!saveTreePrefab("tree3.prefab"))
        return fail(name, "save failed");
    entt::registry reg;
    EntityLifecycle life(reg, 13);
    entt::entity root = PrefabManager::get().spawnAt(reg, prefabPath("tree3.prefab"), 100, 0, 0);
    if (root == entt::null)
        return fail(name, "spawnAt failed");
    if (!nearVec(reg.get<TransformComponent>(root).position, {100, 0, 0}))
        return fail(name, "root position not overridden");
    auto descendants = Hierarchy::collectDescendants(reg, root);
    if (descendants.size() != 2)
        return fail(name, "expected 2 descendants");
    Hierarchy::updateTransformPropagation(reg);
    // Child local (1,0,0) under moved root -> world (101,0,0); grand (101,2,0).
    bool saw_child = false, saw_grand = false;
    for (entt::entity d : descendants)
    {
        glm::vec3 w = reg.get<WorldTransformComponent>(d).position;
        if (nearVec(w, {101, 0, 0})) saw_child = true;
        if (nearVec(w, {101, 2, 0})) saw_grand = true;
    }
    if (!saw_child || !saw_grand)
        return fail(name, "descendant world transforms wrong after root move");
    return pass(name);
}

// v1 single-entity prefab loads through the migrator and spawns.
bool testV1Loads()
{
    const std::string name = "Prefab v1 single-entity loads via migrator";
    EventBus::get().clear();
    const std::string path = prefabPath("legacy.prefab");
    writeFile(path,
        "{\"format\":\"garden_prefab\",\"version\":1,\"name\":\"Legacy\","
        "\"components\":{\"TagComponent\":{\"name\":\"Legacy\"},"
        "\"TransformComponent\":{\"position\":[1,2,3],\"rotation\":[0,0,0],\"scale\":[1,1,1]}}}");
    PrefabData data;
    if (!PrefabManager::loadPrefab(path, data))
        return fail(name, "v1 loadPrefab failed");
    if (data.version != 1 || !data.valid || data.json["version"] != 2 ||
        data.json["nodes"].size() != 1)
        return fail(name, "v1 not migrated to a 1-node v2 document");
    entt::registry reg;
    EntityLifecycle life(reg, 17);
    entt::entity e = PrefabManager::get().spawn(reg, path);
    if (e == entt::null)
        return fail(name, "v1 spawn failed");
    if (!nearVec(reg.get<TransformComponent>(e).position, {1, 2, 3}))
        return fail(name, "v1 transform wrong");
    if (reg.get<TagComponent>(e).name != "Legacy")
        return fail(name, "v1 tag wrong");
    if (!reg.all_of<PrefabInstanceComponent>(e))
        return fail(name, "v1 spawn missing instance component");
    return pass(name);
}

// Version-mismatch and corrupt files are rejected without touching the registry.
bool testRejectedFiles()
{
    const std::string name = "Prefab version-mismatch/corrupt rejected safely";
    EventBus::get().clear();
    entt::registry reg;
    EntityLifecycle life(reg, 19);
    const size_t before = entityCount(reg);

    const std::string future = prefabPath("future.prefab");
    writeFile(future, "{\"format\":\"garden_prefab\",\"version\":99,\"name\":\"Future\"}");
    PrefabData data;
    if (PrefabManager::loadPrefab(future, data))
        return fail(name, "version 99 was accepted");
    if (PrefabManager::get().spawn(reg, future) != entt::null)
        return fail(name, "version 99 spawned an entity");

    const std::string corrupt = prefabPath("corrupt.prefab");
    writeFile(corrupt, "{ this is not json");
    if (PrefabManager::loadPrefab(corrupt, data))
        return fail(name, "corrupt file was accepted");
    if (PrefabManager::get().spawn(reg, corrupt) != entt::null)
        return fail(name, "corrupt file spawned an entity");

    const std::string no_version = prefabPath("noversion.prefab");
    writeFile(no_version, "{\"format\":\"garden_prefab\",\"name\":\"NoVersion\"}");
    if (PrefabManager::loadPrefab(no_version, data))
        return fail(name, "version-less file was accepted");

    if (entityCount(reg) != before)
        return fail(name, "registry mutated by rejected prefabs");
    return pass(name);
}

// Nested prefab: referenced (not flattened), override applies, missing and
// cyclic references degrade to placeholders without failing the whole prefab.
bool testNested()
{
    const std::string name = "Prefab v2 nested reference + missing/cyclic placeholders";
    EventBus::get().clear();
    // Inner prefab: one node.
    {
        entt::registry reg;
        EntityLifecycle life(reg, 23);
        entt::entity inner = life.create("Inner");
        auto& tr = reg.emplace<TransformComponent>(inner);
        tr.position = {1, 2, 3};
        if (!PrefabManager::get().savePrefab(reg, inner, prefabPath("inner.prefab")))
            return fail(name, "inner save failed");
    }
    PrefabData inner_data;
    if (!PrefabManager::loadPrefab(prefabPath("inner.prefab"), inner_data))
        return fail(name, "inner load failed");

    auto makeOuter = [&](const std::string& file, const std::string& nested_source,
                         bool with_override) {
        EntityGuid root_g = generateEntityGuid();
        EntityGuid nest_g = generateEntityGuid();
        nlohmann::json doc;
        doc["format"] = "garden_prefab";
        doc["version"] = 2;
        doc["name"] = "Outer";
        doc["root"] = guidToString(root_g);
        nlohmann::json root_node;
        root_node["guid"] = guidToString(root_g);
        root_node["name"] = "OuterRoot";
        root_node["components"] = {{"TagComponent", {{"name", "OuterRoot"}}},
                                    {"TransformComponent", {{"position", {0, 0, 0}},
                                                            {"rotation", {0, 0, 0}},
                                                            {"scale", {1, 1, 1}}}}};
        root_node["children"] = {guidToString(nest_g)};
        nlohmann::json nest_node;
        nest_node["guid"] = guidToString(nest_g);
        nest_node["name"] = "NestedInner";
        nest_node["parent"] = guidToString(root_g);
        nest_node["children"] = nlohmann::json::array();
        nlohmann::json inst;
        inst["source"] = nested_source;
        inst["overrides"] = nlohmann::json::array();
        if (with_override)
        {
            nlohmann::json o;
            o["node"] = guidToString(inner_data.root_guid);
            o["component"] = "TransformComponent";
            o["property"] = "position";
            o["value"] = nlohmann::json::array({7.0, 8.0, 9.0});
            auto arr = nlohmann::json::array();
            arr.push_back(o);
            inst["overrides"] = arr;
        }
        nest_node["prefab_instance"] = inst;
        doc["nodes"] = {root_node, nest_node};
        doc["sidecars"] = nlohmann::json::object();
        writeFile(prefabPath(file), doc.dump(2));
        return std::pair{root_g, nest_g};
    };

    entt::registry reg;
    EntityLifecycle life(reg, 29);
    // Happy path with an override on the nested root.
    makeOuter("outer.prefab", prefabPath("inner.prefab"), true);
    entt::entity root = PrefabManager::get().spawn(reg, prefabPath("outer.prefab"));
    if (root == entt::null)
        return fail(name, "outer spawn failed");
    auto kids = Hierarchy::collectDescendants(reg, root);
    if (kids.size() != 1)
        return fail(name, "expected exactly the nested root as child");
    if (reg.get<TagComponent>(kids[0]).name != "Inner")
        return fail(name, "nested node did not instantiate the inner prefab");
    if (!nearVec(reg.get<TransformComponent>(kids[0]).position, {7, 8, 9}))
        return fail(name, "nested override not applied");

    // Missing nested source -> placeholder, outer still spawns.
    makeOuter("outer_missing.prefab", prefabPath("does_not_exist.prefab"), false);
    entt::entity root2 = PrefabManager::get().spawn(reg, prefabPath("outer_missing.prefab"));
    if (root2 == entt::null)
        return fail(name, "outer with missing nested failed to spawn");
    auto kids2 = Hierarchy::collectDescendants(reg, root2);
    if (kids2.size() != 1 ||
        reg.get<TagComponent>(kids2[0]).name.find("missing prefab") == std::string::npos)
        return fail(name, "missing nested not replaced by placeholder");

    // Cyclic nesting A <-> B terminates with a placeholder.
    {
        EntityGuid ga = generateEntityGuid(), gb_node = generateEntityGuid();
        nlohmann::json a;
        a["format"] = "garden_prefab"; a["version"] = 2; a["name"] = "A";
        a["root"] = guidToString(ga);
        nlohmann::json an; an["guid"] = guidToString(ga); an["name"] = "A";
        an["components"] = {{"TagComponent", {{"name", "A"}}}};
        an["children"] = {guidToString(gb_node)};
        nlohmann::json ab; ab["guid"] = guidToString(gb_node); ab["name"] = "BLink";
        ab["parent"] = guidToString(ga); ab["children"] = nlohmann::json::array();
        ab["prefab_instance"] = {{"source", prefabPath("cyc_b.prefab")},
                                 {"overrides", nlohmann::json::array()}};
        a["nodes"] = {an, ab}; a["sidecars"] = nlohmann::json::object();
        writeFile(prefabPath("cyc_a.prefab"), a.dump(2));

        EntityGuid gb = generateEntityGuid(), ga_node = generateEntityGuid();
        nlohmann::json b;
        b["format"] = "garden_prefab"; b["version"] = 2; b["name"] = "B";
        b["root"] = guidToString(gb);
        nlohmann::json bn; bn["guid"] = guidToString(gb); bn["name"] = "B";
        bn["components"] = {{"TagComponent", {{"name", "B"}}}};
        bn["children"] = {guidToString(ga_node)};
        nlohmann::json ba; ba["guid"] = guidToString(ga_node); ba["name"] = "ALink";
        ba["parent"] = guidToString(gb); ba["children"] = nlohmann::json::array();
        ba["prefab_instance"] = {{"source", prefabPath("cyc_a.prefab")},
                                 {"overrides", nlohmann::json::array()}};
        b["nodes"] = {bn, ba}; b["sidecars"] = nlohmann::json::object();
        writeFile(prefabPath("cyc_b.prefab"), b.dump(2));
    }
    entt::entity cyc = PrefabManager::get().spawn(reg, prefabPath("cyc_a.prefab"));
    if (cyc == entt::null)
        return fail(name, "cyclic nesting failed to spawn outer root");
    if (Hierarchy::collectDescendants(reg, cyc).empty())
        return fail(name, "cyclic nesting produced no subtree");
    return pass(name);
}
} // namespace

int main()
{
    registerEngineReflection(g_reflection);
    g_reflection.reflect<PrefabLinkComponent>("PrefabLinkComponent");
    PrefabManager::get().initialize(&g_reflection, nullptr);

    g_dir = fs::temp_directory_path() / "garden_prefab_tests";
    std::error_code ec;
    fs::create_directories(g_dir, ec);

    int failures = 0;
    if (!testSubtreeInstantiateTwice()) ++failures;
    if (!testOverrides()) ++failures;
    if (!testSpawnAtRootOnly()) ++failures;
    if (!testV1Loads()) ++failures;
    if (!testRejectedFiles()) ++failures;
    if (!testNested()) ++failures;

    if (failures == 0)
        std::cout << "All PrefabTests passed" << std::endl;
    else
        std::cerr << failures << " PrefabTests FAILED" << std::endl;
    return failures == 0 ? 0 : 1;
}
