#include "PrefabManager.hpp"
#include "Reflection/ReflectionSerializer.hpp"
#include "Reflection/ReflectionRegistry.hpp"
#include "Reflection/ReflectionPropertyOps.hpp"
#include "Components/Components.hpp"
#include "Components/PrefabInstanceComponent.hpp"
#include "Components/HierarchyComponents.hpp"
#include "Components/mesh.hpp"
#include "ECS/Hierarchy.hpp"
#include "Graphics/RenderAPI.hpp"
#include "Assets/AssetManager.hpp"
#include "Utils/Log.hpp"
#include <algorithm>
#include <fstream>
#include <unordered_set>

using json = nlohmann::json;

namespace
{
bool isStructuralComponent(const std::string& name)
{
    // Identity is stored as the node GUID, Parent/Children as node links,
    // PrefabInstance is stamped at instantiation. None are serialized inline.
    return name == "IdentityComponent" || name == "ParentComponent" ||
           name == "ChildrenComponent" || name == "PrefabInstanceComponent";
}

bool parseGuid(const json& value, EntityGuid& out)
{
    if (!value.is_string())
        return false;
    return guidFromString(value.get<std::string>(), out);
}

bool nodeGuid(const json& node, EntityGuid& out)
{
    return node.contains("guid") && parseGuid(node["guid"], out);
}

json filteredComponents(const json& components)
{
    json out = json::object();
    if (!components.is_object())
        return out;
    for (auto& [name, comp] : components.items())
        if (!isStructuralComponent(name))
            out[name] = comp;
    return out;
}

std::vector<PropertyOverride> parseOverrides(const json& arr)
{
    std::vector<PropertyOverride> out;
    if (!arr.is_array())
        return out;
    for (const auto& entry : arr)
    {
        if (!entry.is_object() || !entry.contains("component") || !entry.contains("property"))
        {
            LOG_ENGINE_WARN("PrefabManager — skipping malformed override entry");
            continue;
        }
        PropertyOverride o;
        if (!entry.contains("node") || !parseGuid(entry["node"], o.node))
        {
            LOG_ENGINE_WARN("PrefabManager — skipping override with missing/invalid node guid");
            continue;
        }
        o.component = entry["component"].get<std::string>();
        o.property = entry["property"].get<std::string>();
        o.value = entry.contains("value") ? entry["value"] : json(nullptr);
        out.push_back(std::move(o));
    }
    return out;
}

json serializeOverrides(const std::vector<PropertyOverride>& overrides)
{
    json arr = json::array();
    for (const auto& o : overrides)
    {
        json entry;
        entry["node"] = guidToString(o.node);
        entry["component"] = o.component;
        entry["property"] = o.property;
        entry["value"] = o.value;
        arr.push_back(std::move(entry));
    }
    return arr;
}

// Remap one GUID through the instantiation map. GUIDs outside the map are
// external references and are preserved untouched (plan §3 pass 2).
EntityGuid remapGuid(const EntityGuid& g,
                     const std::unordered_map<EntityGuid, EntityGuid, GuidHash>& map)
{
    if (g.isNull())
        return g;
    auto it = map.find(g);
    return it != map.end() ? it->second : g;
}

void remapEntityRefs(entt::registry& registry, entt::entity entity,
                     const ReflectionRegistry& reflection,
                     const std::unordered_map<EntityGuid, EntityGuid, GuidHash>& guid_map)
{
    for (const auto& desc : reflection.getAll())
    {
        if (isStructuralComponent(desc.name))
            continue;
        if (!desc.has(registry, entity))
            continue;
        void* comp = desc.get(registry, entity);
        if (!comp)
            continue;
        for (const auto& prop : desc.properties)
        {
            if (prop.type == EPropertyType::EntityRef)
            {
                void* field = ReflectionPropertyOps::propertyData(prop, comp);
                if (!field)
                    continue;
                auto* guid = static_cast<EntityGuid*>(field);
                *guid = remapGuid(*guid, guid_map);
            }
            else if (prop.type == EPropertyType::Array &&
                     prop.element_type == EPropertyType::EntityRef &&
                     prop.array_size && prop.array_read_element && prop.array_write_element)
            {
                void* field = ReflectionPropertyOps::propertyData(prop, comp);
                if (!field)
                    continue;
                const size_t count = prop.array_size(field);
                std::vector<unsigned char> elem(prop.element_size ? prop.element_size : 1);
                for (size_t i = 0; i < count; ++i)
                {
                    prop.array_read_element(field, i, elem.data());
                    auto* guid = reinterpret_cast<EntityGuid*>(elem.data());
                    *guid = remapGuid(*guid, guid_map);
                    prop.array_write_element(field, i, elem.data());
                }
            }
        }
    }
}

void applyOverridesToNodes(entt::registry& registry,
                           const std::unordered_map<EntityGuid, entt::entity, GuidHash>& nodes,
                           const std::vector<PropertyOverride>& overrides,
                           const ReflectionRegistry& reflection)
{
    for (const auto& o : overrides)
    {
        auto it = nodes.find(o.node);
        if (it == nodes.end() || it->second == entt::null || !registry.valid(it->second))
        {
            LOG_ENGINE_WARN("PrefabManager — override targets unknown node, skipping");
            continue;
        }
        const ComponentDescriptor* desc = reflection.findByName(o.component.c_str());
        if (!desc)
        {
            LOG_ENGINE_WARN("PrefabManager — override targets unknown component '{}', skipping", o.component);
            continue;
        }
        const PropertyDescriptor* target = nullptr;
        for (const auto& prop : desc->properties)
            if (prop.name == o.property)
                target = &prop;
        if (!target)
        {
            LOG_ENGINE_WARN("PrefabManager — override targets removed property '{}' on '{}', skipping",
                            o.property, o.component);
            continue;
        }
        if (!desc->has(registry, it->second))
            desc->add(registry, it->second);
        void* comp = desc->get(registry, it->second);
        if (!comp || !ReflectionPropertyOps::deserializeProperty(*target, comp, o.value))
            LOG_ENGINE_WARN("PrefabManager — override value rejected for '{}.{}', skipping",
                            o.component, o.property);
    }
}
} // namespace

void PrefabManager::initialize(ReflectionRegistry* reflection, IRenderAPI* render_api)
{
    m_reflection = reflection;
    m_render_api = render_api;
}

// ---------------------------------------------------------------------------
// Save (v2): serialize the whole subtree rooted at `entity`.
// ---------------------------------------------------------------------------
bool PrefabManager::savePrefab(
    entt::registry& registry,
    entt::entity entity,
    const std::string& file_path,
    const std::string& mesh_path,
    const std::string& collider_mesh_path)
{
    if (!registry.valid(entity))
    {
        LOG_ENGINE_ERROR("PrefabManager::savePrefab — invalid entity");
        return false;
    }
    if (!m_reflection)
    {
        LOG_ENGINE_ERROR("PrefabManager::savePrefab — not initialized");
        return false;
    }
    if (registry.all_of<ParentComponent>(entity))
        LOG_ENGINE_WARN("PrefabManager::savePrefab — entity is not a hierarchy root; "
                        "saving the subtree rooted at it");

    // Collect the subtree breadth-first (parents before children). Children
    // are found via the ChildrenComponent mirror, with a ParentComponent
    // scan as fallback so a stale mirror cannot drop a node.
    std::vector<entt::entity> ordered;
    std::unordered_set<entt::entity> visited;
    std::vector<entt::entity> queue{entity};
    visited.insert(entity);
    while (!queue.empty())
    {
        std::vector<entt::entity> next;
        for (entt::entity cur : queue)
        {
            ordered.push_back(cur);
            ensureEntityRef(registry, cur);
            // A nested prefab instance (non-root) is saved by reference:
            // do not descend into its subtree.
            const auto* cur_pic = registry.try_get<PrefabInstanceComponent>(cur);
            if (cur != entity && cur_pic && !cur_pic->prefab_path.empty())
                continue;
            std::vector<entt::entity> children;
            if (auto* cc = registry.try_get<ChildrenComponent>(cur))
                for (const EntityRef& ref : cc->children)
                {
                    entt::entity child = resolveEntityRef(registry, ref);
                    if (child != entt::null && registry.valid(child))
                        children.push_back(child);
                }
            const EntityGuid cur_guid = entityRefOf(registry, cur).guid;
            for (auto [e, parent] : registry.view<ParentComponent>().each())
                if (parent.parent.guid == cur_guid && registry.valid(e))
                    children.push_back(e);
            std::sort(children.begin(), children.end());
            children.erase(std::unique(children.begin(), children.end()), children.end());
            for (entt::entity child : children)
                if (visited.insert(child).second)
                    next.push_back(child);
        }
        queue.swap(next);
    }

    const EntityGuid root_guid = entityRefOf(registry, entity).guid;

    json nodes = json::array();
    json sidecars = json::object();
    for (entt::entity e : ordered)
    {
        const EntityGuid guid = entityRefOf(registry, e).guid;
        json node;
        node["guid"] = guidToString(guid);
        if (auto* tag = registry.try_get<TagComponent>(e))
            node["name"] = tag->name;

        // Parent / children links (GUIDs). Parent links are authoritative.
        if (auto* p = registry.try_get<ParentComponent>(e))
            if (!p->parent.isNull())
                node["parent"] = guidToString(p->parent.guid);
        json child_guids = json::array();
        if (auto* cc = registry.try_get<ChildrenComponent>(e))
            for (const EntityRef& ref : cc->children)
                if (resolveEntityRef(registry, ref) != entt::null)
                    child_guids.push_back(guidToString(ref.guid));
        node["children"] = child_guids;

        // Nested prefab: a non-root node that is itself a prefab instance is
        // stored as a reference, not flattened (plan §2 decision 2).
        const auto* pic = registry.try_get<PrefabInstanceComponent>(e);
        if (e != entity && pic && !pic->prefab_path.empty())
        {
            json inst;
            inst["source"] = pic->prefab_path;
            inst["instance_guid"] = guidToString(pic->instance_guid);
            inst["overrides"] = serializeOverrides(computeOverrides(registry, e));
            node["prefab_instance"] = std::move(inst);
            node["children"] = json::array(); // nested content is not stored here
            nodes.push_back(std::move(node));
            continue;
        }

        json entity_json = ReflectionSerializer::serializeEntity(registry, e, *m_reflection);
        node["components"] = entity_json.contains("components")
            ? filteredComponents(entity_json["components"]) : json::object();
        nodes.push_back(std::move(node));
    }

    // Non-reflected sidecars, root node only (editor supplies the paths).
    if (!mesh_path.empty() || !collider_mesh_path.empty())
    {
        json sidecar;
        if (!mesh_path.empty())
        {
            sidecar["path"] = mesh_path;
            if (auto* mc = registry.try_get<MeshComponent>(entity))
            {
                if (mc->m_mesh)
                {
                    sidecar["culling"]      = mc->m_mesh->culling;
                    sidecar["transparent"]  = mc->m_mesh->transparent;
                    sidecar["visible"]      = mc->m_mesh->visible;
                    sidecar["casts_shadow"] = mc->m_mesh->casts_shadow;
                    sidecar["force_lod"]    = mc->m_mesh->force_lod;
                }
                else
                {
                    sidecar["culling"] = true; sidecar["transparent"] = false;
                    sidecar["visible"] = true; sidecar["casts_shadow"] = true;
                    sidecar["force_lod"] = -1;
                }
            }
        }
        if (!collider_mesh_path.empty())
            sidecar["collider_mesh_path"] = collider_mesh_path;
        sidecars[guidToString(root_guid)] = std::move(sidecar);
    }

    json prefab;
    prefab["format"] = "garden_prefab";
    prefab["version"] = kPrefabVersion;
    if (auto* tag = registry.try_get<TagComponent>(entity))
        prefab["name"] = tag->name;
    else
        prefab["name"] = "Unnamed Prefab";
    prefab["root"] = guidToString(root_guid);
    prefab["nodes"] = std::move(nodes);
    prefab["sidecars"] = std::move(sidecars);

    std::ofstream file(file_path);
    if (!file.is_open())
    {
        LOG_ENGINE_ERROR("PrefabManager::savePrefab — could not open file: {}", file_path);
        return false;
    }
    file << prefab.dump(2);
    file.close();
    LOG_ENGINE_TRACE("Prefab saved: {}", file_path);
    return true;
}

// ---------------------------------------------------------------------------
// Load + version gate + v1 migration (plan §5).
// ---------------------------------------------------------------------------
namespace
{
bool migratePrefabV1toV2(const json& in, json& out)
{
    const EntityGuid guid = generateEntityGuid();
    json node;
    node["guid"] = guidToString(guid);
    if (in.contains("name") && in["name"].is_string())
        node["name"] = in["name"];
    node["components"] = in.contains("components") ? filteredComponents(in["components"])
                                                   : json::object();
    node["children"] = json::array();

    json sidecars = json::object();
    json sidecar;
    bool has_sidecar = false;
    if (in.contains("mesh") && in["mesh"].is_object())
    {
        sidecar.update(in["mesh"]);
        has_sidecar = true;
    }
    if (in.contains("collider") && in["collider"].is_object() &&
        in["collider"].contains("mesh_path"))
    {
        sidecar["collider_mesh_path"] = in["collider"]["mesh_path"];
        has_sidecar = true;
    }
    if (has_sidecar)
        sidecars[guidToString(guid)] = std::move(sidecar);

    out = json::object();
    out["format"] = "garden_prefab";
    out["version"] = 2;
    out["name"] = in.contains("name") ? in["name"] : json("Unnamed");
    out["root"] = guidToString(guid);
    out["nodes"] = json::array({std::move(node)});
    out["sidecars"] = std::move(sidecars);
    return true;
}

bool validateV2(const json& doc, EntityGuid& root_out)
{
    if (!doc.contains("nodes") || !doc["nodes"].is_array() || doc["nodes"].empty())
        return false;
    if (!doc.contains("root") || !parseGuid(doc["root"], root_out))
        return false;
    std::unordered_set<EntityGuid, GuidHash> seen;
    bool root_found = false;
    for (const auto& node : doc["nodes"])
    {
        EntityGuid g;
        if (!node.is_object() || !nodeGuid(node, g))
            return false;
        if (!seen.insert(g).second)
            return false; // duplicate node guid
        if (g == root_out)
            root_found = true;
    }
    return root_found;
}
} // namespace

bool PrefabManager::normalizePrefabJson(const json& in, PrefabData& out_data)
{
    if (!in.is_object() || !in.contains("format") || in["format"] != "garden_prefab")
    {
        LOG_ENGINE_ERROR("PrefabManager — not a valid prefab document (bad format)");
        return false;
    }
    if (!in.contains("version") || !in["version"].is_number_integer())
    {
        LOG_ENGINE_ERROR("PrefabManager — prefab has no version field; rejected");
        return false;
    }
    const int version = in["version"].get<int>();
    if (version > kPrefabVersion)
    {
        LOG_ENGINE_ERROR("PrefabManager — prefab version {} is newer than supported {}; rejected",
                         version, kPrefabVersion);
        return false;
    }
    if (version < 1)
    {
        LOG_ENGINE_ERROR("PrefabManager — prefab version {} is invalid; rejected", version);
        return false;
    }

    PrefabData result;
    result.version = version;
    if (version == 1)
    {
        LOG_ENGINE_INFO("PrefabManager — migrating prefab from v1 to v2");
        if (!migratePrefabV1toV2(in, result.json))
            return false;
    }
    else
    {
        result.json = in;
    }

    if (!validateV2(result.json, result.root_guid))
    {
        LOG_ENGINE_ERROR("PrefabManager — prefab failed v2 validation (nodes/root); rejected");
        return false;
    }
    result.name = result.json.contains("name") && result.json["name"].is_string()
        ? result.json["name"].get<std::string>() : "Unnamed";
    result.valid = true;
    out_data = std::move(result);
    return true;
}

bool PrefabManager::loadPrefab(const std::string& file_path, PrefabData& out_data)
{
    std::string resolved = Assets::AssetManager::get().resolveAssetPath(file_path);
    std::ifstream file(resolved);
    if (!file.is_open())
    {
        LOG_ENGINE_ERROR("PrefabManager::loadPrefab — could not open: {}", file_path);
        return false;
    }
    // Non-throwing parse: the engine builds without exception handling.
    json parsed = json::parse(file, nullptr, false);
    if (parsed.is_discarded())
    {
        LOG_ENGINE_ERROR("PrefabManager::loadPrefab — parse error in {}", file_path);
        return false;
    }
    if (!normalizePrefabJson(parsed, out_data))
    {
        LOG_ENGINE_ERROR("PrefabManager::loadPrefab — rejected prefab: {}", file_path);
        return false;
    }
    out_data.path = file_path;
    return true;
}

// ---------------------------------------------------------------------------
// Instantiation (plan §3).
// ---------------------------------------------------------------------------
namespace
{
struct NodeInfo
{
    EntityGuid guid;
    EntityGuid parent; // null for root / orphan
    const json* node = nullptr;
    bool nested = false;
};

void applySidecar(entt::registry& registry, entt::entity entity, const json& sidecar,
                  IRenderAPI* render_api)
{
    if (sidecar.contains("path") && sidecar["path"].is_string())
    {
        std::string mesh_file = Assets::AssetManager::get().resolveAssetPath(
            sidecar["path"].get<std::string>());
        if (!mesh_file.empty())
        {
            auto mesh_ptr = std::make_shared<mesh>(mesh_file, render_api);
            if (mesh_ptr->is_valid)
            {
                if (sidecar.contains("culling"))      mesh_ptr->culling      = sidecar["culling"].get<bool>();
                if (sidecar.contains("transparent"))  mesh_ptr->transparent  = sidecar["transparent"].get<bool>();
                if (sidecar.contains("visible"))      mesh_ptr->visible      = sidecar["visible"].get<bool>();
                if (sidecar.contains("casts_shadow")) mesh_ptr->casts_shadow = sidecar["casts_shadow"].get<bool>();
                if (sidecar.contains("force_lod"))    mesh_ptr->force_lod    = sidecar["force_lod"].get<int>();
                if (render_api)
                    mesh_ptr->uploadToGPU(render_api);
                registry.get_or_emplace<MeshComponent>(entity).m_mesh = mesh_ptr;
            }
            else
                LOG_ENGINE_ERROR("PrefabManager — failed to load mesh: {}", mesh_file);
        }
    }
    if (sidecar.contains("collider_mesh_path") && sidecar["collider_mesh_path"].is_string())
    {
        std::string collider_file = Assets::AssetManager::get().resolveAssetPath(
            sidecar["collider_mesh_path"].get<std::string>());
        if (!collider_file.empty())
        {
            auto col_mesh = std::make_shared<mesh>(collider_file, render_api);
            if (col_mesh->is_valid)
            {
                if (render_api)
                    col_mesh->uploadToGPU(render_api);
                registry.get_or_emplace<ColliderComponent>(entity).m_mesh = col_mesh;
            }
            else
                LOG_ENGINE_ERROR("PrefabManager — failed to load collider mesh: {}", collider_file);
        }
    }
}
} // namespace

PrefabInstantiateResult PrefabManager::instantiate(
    entt::registry& registry, const PrefabData& prefab, const PrefabInstantiateOptions& options)
{
    std::vector<std::string> stack;
    if (!prefab.path.empty())
        stack.push_back(prefab.path);
    return instantiateInternal(registry, prefab, options, 0, stack);
}

PrefabInstantiateResult PrefabManager::instantiateInternal(
    entt::registry& registry, const PrefabData& prefab,
    const PrefabInstantiateOptions& options, int depth,
    std::vector<std::string>& source_stack)
{
    PrefabInstantiateResult result;
    if (!m_reflection)
    {
        LOG_ENGINE_ERROR("PrefabManager::instantiate — not initialized");
        return result;
    }
    // Validate fully before creating anything: a rejected prefab must leave
    // the registry unchanged (plan P6).
    EntityGuid root_check;
    if (!prefab.valid || !validateV2(prefab.json, root_check) || root_check != prefab.root_guid)
    {
        LOG_ENGINE_ERROR("PrefabManager::instantiate — invalid prefab data; nothing created");
        return result;
    }

    std::vector<NodeInfo> nodes;
    std::unordered_map<EntityGuid, size_t, GuidHash> index_of;
    for (const auto& node : prefab.json["nodes"])
    {
        NodeInfo info;
        nodeGuid(node, info.guid);
        info.node = &node;
        if (node.contains("parent"))
            parseGuid(node["parent"], info.parent);
        info.nested = node.contains("prefab_instance") && node["prefab_instance"].is_object();
        index_of[info.guid] = nodes.size();
        nodes.push_back(info);
    }
    // Parent GUIDs that are not in this file: node becomes a root (warn).
    for (auto& info : nodes)
        if (!info.parent.isNull() && index_of.find(info.parent) == index_of.end())
        {
            LOG_ENGINE_WARN("PrefabManager — node has orphan parent GUID; treated as root");
            info.parent = EntityGuid{};
        }

    // Topological order: parents before children.
    std::vector<NodeInfo> ordered;
    std::unordered_set<EntityGuid, GuidHash> placed;
    while (ordered.size() < nodes.size())
    {
        bool progress = false;
        for (const auto& info : nodes)
        {
            if (placed.count(info.guid))
                continue;
            if (info.parent.isNull() || placed.count(info.parent))
            {
                ordered.push_back(info);
                placed.insert(info.guid);
                progress = true;
            }
        }
        if (!progress)
        {
            LOG_ENGINE_ERROR("PrefabManager::instantiate — hierarchy cycle in prefab; nothing created");
            return result;
        }
    }

    const EntityGuid instance_guid = generateEntityGuid();
    std::unordered_map<EntityGuid, EntityGuid, GuidHash> guid_map; // file -> fresh
    std::unordered_map<EntityGuid, entt::entity, GuidHash> ent_map; // file -> entity
    std::vector<entt::entity> local_entities; // created here (not nested subtrees)

    auto freshEntity = [&](const std::string& tag_name) {
        entt::entity e = registry.create();
        EntityGuid g;
        do { g = generateEntityGuid(); } while (resolveEntityRef(registry, EntityRef{g}) != entt::null);
        registry.emplace<IdentityComponent>(e, IdentityComponent{g});
        if (!tag_name.empty())
            registry.emplace_or_replace<TagComponent>(e, TagComponent{tag_name});
        return e;
    };

    // Pass 1: create entities (nested prefabs recurse and graft their root).
    for (const auto& info : ordered)
    {
        const json& node = *info.node;
        const std::string node_name = node.contains("name") && node["name"].is_string()
            ? node["name"].get<std::string>() : std::string{};
        if (info.nested)
        {
            const json& inst = node["prefab_instance"];
            const std::string source = inst.contains("source") && inst["source"].is_string()
                ? inst["source"].get<std::string>() : std::string{};
            entt::entity grafted = entt::null;
            const bool cyclic = std::find(source_stack.begin(), source_stack.end(), source) != source_stack.end();
            if (source.empty() || cyclic || depth + 1 > kMaxNestedDepth)
            {
                LOG_ENGINE_ERROR("PrefabManager — nested prefab '{}' {} (depth {}); placeholder substituted",
                                 source, cyclic ? "is cyclic" : "exceeds depth limit", depth + 1);
            }
            else
            {
                PrefabData nested_data;
                if (!loadPrefab(source, nested_data))
                {
                    LOG_ENGINE_ERROR("PrefabManager — nested prefab missing: {}; placeholder substituted", source);
                }
                else
                {
                    PrefabInstantiateOptions nested_opts;
                    if (inst.contains("overrides"))
                        nested_opts.overrides = parseOverrides(inst["overrides"]);
                    source_stack.push_back(source);
                    PrefabInstantiateResult sub = instantiateInternal(
                        registry, nested_data, nested_opts, depth + 1, source_stack);
                    source_stack.pop_back();
                    if (sub.success)
                        grafted = sub.root;
                    else
                        LOG_ENGINE_ERROR("PrefabManager — nested prefab failed: {}; placeholder substituted", source);
                }
            }
            if (grafted == entt::null)
                grafted = freshEntity(node_name.empty() ? "MissingPrefab" : node_name + " (missing prefab)");
            ent_map[info.guid] = grafted;
            guid_map[info.guid] = entityRefOf(registry, grafted).guid;
            continue;
        }
        entt::entity e = freshEntity(node_name);
        ent_map[info.guid] = e;
        guid_map[info.guid] = entityRefOf(registry, e).guid;
        local_entities.push_back(e);
    }

    // Pass 2: deserialize components, then remap internal EntityRefs.
    for (const auto& info : ordered)
    {
        if (info.nested)
            continue;
        entt::entity e = ent_map[info.guid];
        json entity_json;
        entity_json["components"] = (info.node->contains("components"))
            ? filteredComponents((*info.node)["components"]) : json::object();
        ReflectionSerializer::deserializeEntity(registry, e, entity_json, *m_reflection);
    }
    for (entt::entity e : local_entities)
        remapEntityRefs(registry, e, *m_reflection, guid_map);

    // Sidecars (mesh/collider) for local nodes.
    if (prefab.json.contains("sidecars") && prefab.json["sidecars"].is_object())
        for (const auto& info : ordered)
        {
            if (info.nested)
                continue;
            const std::string key = guidToString(info.guid);
            if (prefab.json["sidecars"].contains(key))
                applySidecar(registry, ent_map[info.guid], prefab.json["sidecars"][key], m_render_api);
        }

    // Pass 3: rebuild hierarchy links through the Hierarchy API (maintains
    // Parent + Children mirrors and world-transform components).
    for (const auto& info : ordered)
    {
        if (info.parent.isNull())
            continue;
        auto pit = ent_map.find(info.parent);
        if (pit != ent_map.end())
            Hierarchy::attach(registry, ent_map[info.guid], pit->second, false);
    }

    // Stamp instance linkage on every locally created node.
    for (const auto& info : ordered)
    {
        if (info.nested)
            continue;
        PrefabInstanceComponent pic;
        pic.prefab_path = prefab.path;
        pic.instance_guid = instance_guid;
        pic.source_root_guid = prefab.root_guid;
        pic.source_node_guid = info.guid;
        registry.emplace_or_replace<PrefabInstanceComponent>(ent_map[info.guid], std::move(pic));
    }

    // Pass 4: overrides — file-stored first, caller-supplied win (file order).
    if (prefab.json.contains("overrides"))
        applyOverridesToNodes(registry, ent_map, parseOverrides(prefab.json["overrides"]), *m_reflection);
    applyOverridesToNodes(registry, ent_map, options.overrides, *m_reflection);

    // Spawn transform applies to the root only.
    auto rit = ent_map.find(prefab.root_guid);
    if (rit == ent_map.end())
    {
        LOG_ENGINE_ERROR("PrefabManager::instantiate — root node missing after instantiation");
        return result;
    }
    result.root = rit->second;
    if (options.has_spawn_transform)
        if (auto* t = registry.try_get<TransformComponent>(result.root))
            t->position = options.spawn_position;
    result.nodes = ent_map;
    result.success = true;
    return result;
}

// ---------------------------------------------------------------------------
// Overrides: compute / revert / apply (plan §3).
// ---------------------------------------------------------------------------
namespace
{
bool collectInstance(entt::registry& registry, entt::entity root,
                     PrefabInstanceComponent& out_root_pic,
                     std::unordered_map<EntityGuid, entt::entity, GuidHash>& by_source_node)
{
    auto* pic = registry.try_get<PrefabInstanceComponent>(root);
    if (!pic)
        return false;
    out_root_pic = *pic;
    for (auto [e, inst] : registry.view<PrefabInstanceComponent>().each())
        if (inst.instance_guid == pic->instance_guid)
            by_source_node[inst.source_node_guid] = e;
    return true;
}
} // namespace

std::vector<PropertyOverride> PrefabManager::computeOverrides(
    entt::registry& registry, entt::entity instance_root)
{
    std::vector<PropertyOverride> out;
    if (!m_reflection)
        return out;
    PrefabInstanceComponent root_pic;
    std::unordered_map<EntityGuid, entt::entity, GuidHash> by_source;
    if (!collectInstance(registry, instance_root, root_pic, by_source))
        return out;
    PrefabData source;
    if (!loadPrefab(root_pic.prefab_path, source))
        return out;

    for (const auto& node : source.json["nodes"])
    {
        EntityGuid src_guid;
        if (!nodeGuid(node, src_guid))
            continue;
        auto it = by_source.find(src_guid);
        if (it == by_source.end())
            continue;
        if (node.contains("prefab_instance"))
            continue; // nested content diffs belong to the nested instance
        entt::entity e = it->second;
        const json src_components = node.contains("components") ? node["components"] : json::object();
        for (const auto& desc : m_reflection->getAll())
        {
            if (isStructuralComponent(desc.name))
                continue;
            const bool in_source = src_components.is_object() && src_components.contains(desc.name);
            if (!desc.has(registry, e))
                continue;
            void* comp = desc.get(registry, e);
            if (!comp)
                continue;
            json live = ReflectionSerializer::serializeComponent(desc, comp);
            for (const auto& prop : desc.properties)
            {
                json live_value = live.contains(prop.name) ? live[prop.name] : json(nullptr);
                json src_value = (in_source && src_components[desc.name].contains(prop.name))
                    ? src_components[desc.name][prop.name] : json(nullptr);
                if (live_value != src_value)
                    out.push_back(PropertyOverride{src_guid, desc.name, prop.name, live_value});
            }
        }
    }
    return out;
}

bool PrefabManager::revertOverrides(entt::registry& registry, entt::entity instance_root)
{
    if (!m_reflection)
        return false;
    PrefabInstanceComponent root_pic;
    std::unordered_map<EntityGuid, entt::entity, GuidHash> by_source;
    if (!collectInstance(registry, instance_root, root_pic, by_source))
        return false;
    PrefabData source;
    if (!loadPrefab(root_pic.prefab_path, source))
        return false;

    std::unordered_map<EntityGuid, EntityGuid, GuidHash> guid_map;
    for (const auto& [src_guid, e] : by_source)
        guid_map[src_guid] = entityRefOf(registry, e).guid;

    std::unordered_map<EntityGuid, entt::entity, GuidHash> ent_map;
    for (const auto& node : source.json["nodes"])
    {
        EntityGuid src_guid;
        if (!nodeGuid(node, src_guid) || node.contains("prefab_instance"))
            continue;
        auto it = by_source.find(src_guid);
        if (it == by_source.end())
            continue;
        entt::entity e = it->second;
        ent_map[src_guid] = e;
        json entity_json;
        entity_json["components"] = node.contains("components")
            ? filteredComponents(node["components"]) : json::object();
        ReflectionSerializer::deserializeEntity(registry, e, entity_json, *m_reflection);
        remapEntityRefs(registry, e, *m_reflection, guid_map);
    }
    if (source.json.contains("overrides"))
        applyOverridesToNodes(registry, ent_map, parseOverrides(source.json["overrides"]), *m_reflection);
    return true;
}

bool PrefabManager::applyOverrides(entt::registry& registry, entt::entity instance_root)
{
    if (!m_reflection)
        return false;
    auto* pic = registry.try_get<PrefabInstanceComponent>(instance_root);
    if (!pic)
        return false;
    PrefabData source;
    if (!loadPrefab(pic->prefab_path, source))
        return false;
    json doc = source.json;
    doc["overrides"] = serializeOverrides(computeOverrides(registry, instance_root));
    const std::string resolved = Assets::AssetManager::get().resolveAssetPath(pic->prefab_path);
    std::ofstream file(resolved);
    if (!file.is_open())
    {
        LOG_ENGINE_ERROR("PrefabManager::applyOverrides — could not open: {}", pic->prefab_path);
        return false;
    }
    file << doc.dump(2);
    file.close();
    LOG_ENGINE_INFO("PrefabManager::applyOverrides — wrote overrides to {}", pic->prefab_path);
    return true;
}

// ---------------------------------------------------------------------------
// spawn / spawnAt: v1-compatible entry points over instantiate().
// ---------------------------------------------------------------------------
entt::entity PrefabManager::spawn(entt::registry& registry, const std::string& prefab_path)
{
    if (!m_reflection)
    {
        LOG_ENGINE_ERROR("PrefabManager::spawn — not initialized");
        return entt::null;
    }
    PrefabData data;
    if (!loadPrefab(prefab_path, data))
        return entt::null;
    PrefabInstantiateResult r = instantiate(registry, data);
    if (!r.success)
        return entt::null;
    LOG_ENGINE_TRACE("Prefab spawned: {} -> entity {}", prefab_path, static_cast<uint32_t>(r.root));
    return r.root;
}

entt::entity PrefabManager::spawnAt(entt::registry& registry, const std::string& prefab_path,
                                    float x, float y, float z)
{
    if (!m_reflection)
    {
        LOG_ENGINE_ERROR("PrefabManager::spawnAt — not initialized");
        return entt::null;
    }
    PrefabData data;
    if (!loadPrefab(prefab_path, data))
        return entt::null;
    PrefabInstantiateOptions opts;
    opts.has_spawn_transform = true;
    opts.spawn_position = glm::vec3(x, y, z);
    PrefabInstantiateResult r = instantiate(registry, data, opts);
    return r.success ? r.root : entt::null;
}
