#pragma once

#include "EngineExport.h"
#include <string>
#include <unordered_map>
#include <vector>
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include "json.hpp"
#include "Components/IdentityComponent.hpp"

class ReflectionRegistry;
class IRenderAPI;

// One property override, addressed by the source prefab's node GUID
// (prefabs_serialization.md §2 decision 3).
struct PropertyOverride {
    EntityGuid node;              // source node GUID the override targets
    std::string component;        // reflected component name
    std::string property;         // reflected property name
    nlohmann::json value;         // encoded exactly as in component JSON
};

struct PrefabData {
    nlohmann::json json;          // validated current-format document after load
    std::string path;
    std::string name;
    int version = 0;              // validated current format version
    EntityGuid root_guid;
    bool valid = false;
};

struct PrefabInstantiateResult {
    entt::entity root = entt::null;
    // Source (file) GUID -> live entity, for every instantiated node
    // (including nested prefab roots, keyed by their nested node GUID).
    std::unordered_map<EntityGuid, entt::entity, GuidHash> nodes;
    bool success = false;
};

struct PrefabInstantiateOptions {
    std::vector<PropertyOverride> overrides;
    // When true, spawn_position replaces the root node's local position.
    bool has_spawn_transform = false;
    glm::vec3 spawn_position{0.0f};
};

class ENGINE_API PrefabManager
{
public:
    static constexpr int kPrefabVersion = 2;
    static constexpr int kMaxNestedDepth = 16;

    static PrefabManager& get()
    {
        static PrefabManager instance;
        return instance;
    }

    // Call once during engine init — stores the pointers so spawn() doesn't need them.
    void initialize(ReflectionRegistry* reflection, IRenderAPI* render_api);

    // Save an entity's whole subtree (root + descendants via Children) as a
    // v2 .prefab JSON file. mesh_path / collider_mesh_path are string paths
    // for the non-reflectable MeshComponent and ColliderComponent of the
    // root node (from the editor's mesh_path_cache); they are stored in the
    // v2 "sidecars" block. Saving from a non-root saves its subtree as-is;
    // the caller is responsible for passing the intended root.
    bool savePrefab(
        entt::registry& registry,
        entt::entity entity,
        const std::string& file_path,
        const std::string& mesh_path = "",
        const std::string& collider_mesh_path = "");

    // Load a prefab from disk (does not create an entity). Checks the
    // format and version must exactly match the current prefab format; anything
    // malformed or from another version fails.
    static bool loadPrefab(
        const std::string& file_path,
        PrefabData& out_data);

    // Validate a parsed current-format prefab document. Returns false (and does
    // not modify out) when the document is not a supported prefab.
    static bool normalizePrefabJson(const nlohmann::json& in, PrefabData& out_data);

    // Instantiate a loaded prefab: fresh GUIDs for every node, internal
    // EntityRef references remapped into the new instance, external
    // references preserved, hierarchy rebuilt, overrides applied last.
    PrefabInstantiateResult instantiate(
        entt::registry& registry,
        const PrefabData& prefab,
        const PrefabInstantiateOptions& options = {});

    // Diff every reflected property of the instance against its source
    // prefab. Returns the override list that would reproduce the instance.
    std::vector<PropertyOverride> computeOverrides(
        entt::registry& registry,
        entt::entity instance_root);

    // Re-apply source values (plus the source file's stored overrides) to a
    // live instance, discarding local edits.
    bool revertOverrides(entt::registry& registry, entt::entity instance_root);

    // Push the instance's computed overrides back into its source file so
    // future instantiations pick them up. Editor-only operation.
    bool applyOverrides(entt::registry& registry, entt::entity instance_root);

    // Spawn an entity subtree from a prefab file. Returns the instance root.
    entt::entity spawn(
        entt::registry& registry,
        const std::string& prefab_path);

    // Spawn with explicit root position override (root node only).
    entt::entity spawnAt(
        entt::registry& registry,
        const std::string& prefab_path,
        float x, float y, float z);

private:
    PrefabManager() = default;
    ~PrefabManager() = default;
    PrefabManager(const PrefabManager&) = delete;
    PrefabManager& operator=(const PrefabManager&) = delete;

    PrefabInstantiateResult instantiateInternal(
        entt::registry& registry,
        const PrefabData& prefab,
        const PrefabInstantiateOptions& options,
        int depth,
        std::vector<std::string>& source_stack,
        std::vector<entt::entity>& created);

    ReflectionRegistry* m_reflection = nullptr;
    IRenderAPI* m_render_api = nullptr;
};
