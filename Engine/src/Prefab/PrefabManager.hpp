#pragma once

#include "EngineExport.h"
#include <string>
#include <entt/entt.hpp>
#include "json.hpp"

class ReflectionRegistry;
class IRenderAPI;

struct PrefabData {
    nlohmann::json json;
    std::string path;
    std::string name;
};

class ENGINE_API PrefabManager
{
public:
    static PrefabManager& get()
    {
        static PrefabManager instance;
        return instance;
    }

    // Call once during engine init — stores the pointers so spawn() doesn't need them.
    void initialize(ReflectionRegistry* reflection, IRenderAPI* render_api);

    // Save an entity as a .prefab JSON file.
    // mesh_path / collider_mesh_path are string paths for the non-reflectable
    // MeshComponent and ColliderComponent (from the editor's mesh_path_cache).
    bool savePrefab(
        entt::registry& registry,
        entt::entity entity,
        const std::string& file_path,
        const std::string& mesh_path = "",
        const std::string& collider_mesh_path = "");

    // Load a prefab from disk (does not create an entity).
    static bool loadPrefab(
        const std::string& file_path,
        PrefabData& out_data);

    // Spawn an entity from a prefab file.
    entt::entity spawn(
        entt::registry& registry,
        const std::string& prefab_path);

    // Spawn with explicit position override.
    entt::entity spawnAt(
        entt::registry& registry,
        const std::string& prefab_path,
        float x, float y, float z);

private:
    PrefabManager() = default;
    ~PrefabManager() = default;
    PrefabManager(const PrefabManager&) = delete;
    PrefabManager& operator=(const PrefabManager&) = delete;

    ReflectionRegistry* m_reflection = nullptr;
    IRenderAPI* m_render_api = nullptr;
};
