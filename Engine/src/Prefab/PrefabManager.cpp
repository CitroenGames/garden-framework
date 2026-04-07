#include "PrefabManager.hpp"
#include "Reflection/ReflectionSerializer.hpp"
#include "Reflection/ReflectionRegistry.hpp"
#include "Components/Components.hpp"
#include "Components/PrefabInstanceComponent.hpp"
#include "Components/mesh.hpp"
#include "Graphics/RenderAPI.hpp"
#include "Assets/AssetManager.hpp"
#include "Utils/Log.hpp"
#include <fstream>

using json = nlohmann::json;

void PrefabManager::initialize(ReflectionRegistry* reflection, IRenderAPI* render_api)
{
    m_reflection = reflection;
    m_render_api = render_api;
}

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

    // Serialize reflected components using existing ReflectionSerializer
    json entity_json = ReflectionSerializer::serializeEntity(registry, entity, *m_reflection);

    // Build the prefab document
    json prefab;
    prefab["format"] = "garden_prefab";
    prefab["version"] = 1;

    // Use the TagComponent name as the prefab name
    if (auto* tag = registry.try_get<TagComponent>(entity))
        prefab["name"] = tag->name;
    else
        prefab["name"] = "Unnamed Prefab";

    // The "components" block from ReflectionSerializer
    if (entity_json.contains("components"))
        prefab["components"] = entity_json["components"];
    else
        prefab["components"] = json::object();

    // Remove PrefabInstanceComponent from saved data — a prefab file should not
    // contain a self-referencing prefab instance tag
    prefab["components"].erase("PrefabInstanceComponent");

    // Handle non-reflectable MeshComponent
    if (!mesh_path.empty())
    {
        json mesh_json;
        mesh_json["path"] = mesh_path;

        // Read rendering properties from the live mesh if available
        if (auto* mc = registry.try_get<MeshComponent>(entity))
        {
            if (mc->m_mesh)
            {
                mesh_json["culling"]      = mc->m_mesh->culling;
                mesh_json["transparent"]  = mc->m_mesh->transparent;
                mesh_json["visible"]      = mc->m_mesh->visible;
                mesh_json["casts_shadow"] = mc->m_mesh->casts_shadow;
                mesh_json["force_lod"]    = mc->m_mesh->force_lod;
            }
            else
            {
                mesh_json["culling"]      = true;
                mesh_json["transparent"]  = false;
                mesh_json["visible"]      = true;
                mesh_json["casts_shadow"] = true;
                mesh_json["force_lod"]    = -1;
            }
        }

        prefab["mesh"] = mesh_json;
    }

    // Handle non-reflectable ColliderComponent
    if (!collider_mesh_path.empty())
    {
        json collider_json;
        collider_json["mesh_path"] = collider_mesh_path;
        prefab["collider"] = collider_json;
    }

    // Write to disk
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

bool PrefabManager::loadPrefab(
    const std::string& file_path,
    PrefabData& out_data)
{
    std::string resolved = Assets::AssetManager::get().resolveAssetPath(file_path);
    std::ifstream file(resolved);
    if (!file.is_open())
    {
        LOG_ENGINE_ERROR("PrefabManager::loadPrefab — could not open: {}", file_path);
        return false;
    }

    try
    {
        out_data.json = json::parse(file);
    }
    catch (const json::parse_error& e)
    {
        LOG_ENGINE_ERROR("PrefabManager::loadPrefab — parse error in {}: {}", file_path, e.what());
        return false;
    }

    if (!out_data.json.contains("format") || out_data.json["format"] != "garden_prefab")
    {
        LOG_ENGINE_ERROR("PrefabManager::loadPrefab — not a valid prefab file: {}", file_path);
        return false;
    }

    out_data.path = file_path;
    if (out_data.json.contains("name"))
        out_data.name = out_data.json["name"].get<std::string>();
    else
        out_data.name = "Unnamed";

    return true;
}

entt::entity PrefabManager::spawn(
    entt::registry& registry,
    const std::string& prefab_path)
{
    if (!m_reflection)
    {
        LOG_ENGINE_ERROR("PrefabManager::spawn — not initialized");
        return entt::null;
    }

    PrefabData data;
    if (!loadPrefab(prefab_path, data))
        return entt::null;

    // Create entity
    auto entity = registry.create();

    // Deserialize reflected components
    // ReflectionSerializer expects {"components": {...}} at the top level
    json entity_json;
    if (data.json.contains("components"))
        entity_json["components"] = data.json["components"];
    else
        entity_json["components"] = json::object();

    ReflectionSerializer::deserializeEntity(registry, entity, entity_json, *m_reflection);

    // Load mesh (non-reflectable)
    if (data.json.contains("mesh") && data.json["mesh"].contains("path"))
    {
        const auto& mesh_json = data.json["mesh"];
        std::string mesh_file = Assets::AssetManager::get().resolveAssetPath(
            mesh_json["path"].get<std::string>());

        if (!mesh_file.empty())
        {
            auto mesh_ptr = std::make_shared<mesh>(mesh_file, m_render_api);
            if (mesh_ptr->is_valid)
            {
                // Apply rendering properties from prefab
                if (mesh_json.contains("culling"))      mesh_ptr->culling      = mesh_json["culling"].get<bool>();
                if (mesh_json.contains("transparent"))  mesh_ptr->transparent  = mesh_json["transparent"].get<bool>();
                if (mesh_json.contains("visible"))      mesh_ptr->visible      = mesh_json["visible"].get<bool>();
                if (mesh_json.contains("casts_shadow")) mesh_ptr->casts_shadow = mesh_json["casts_shadow"].get<bool>();
                if (mesh_json.contains("force_lod"))    mesh_ptr->force_lod    = mesh_json["force_lod"].get<int>();

                if (m_render_api)
                    mesh_ptr->uploadToGPU(m_render_api);

                auto& mc = registry.get_or_emplace<MeshComponent>(entity);
                mc.m_mesh = mesh_ptr;
            }
            else
            {
                LOG_ENGINE_ERROR("PrefabManager::spawn — failed to load mesh: {}", mesh_file);
            }
        }
    }

    // Load collider mesh (non-reflectable)
    if (data.json.contains("collider") && data.json["collider"].contains("mesh_path"))
    {
        std::string collider_file = Assets::AssetManager::get().resolveAssetPath(
            data.json["collider"]["mesh_path"].get<std::string>());
        if (!collider_file.empty())
        {
            auto col_mesh = std::make_shared<mesh>(collider_file, m_render_api);
            if (col_mesh->is_valid)
            {
                if (m_render_api)
                    col_mesh->uploadToGPU(m_render_api);

                auto& cc = registry.get_or_emplace<ColliderComponent>(entity);
                cc.m_mesh = col_mesh;
            }
            else
            {
                LOG_ENGINE_ERROR("PrefabManager::spawn — failed to load collider mesh: {}", collider_file);
            }
        }
    }

    // Attach prefab instance tag
    registry.emplace_or_replace<PrefabInstanceComponent>(entity, PrefabInstanceComponent{prefab_path});

    LOG_ENGINE_TRACE("Prefab spawned: {} -> entity {}", prefab_path, static_cast<uint32_t>(entity));
    return entity;
}

entt::entity PrefabManager::spawnAt(
    entt::registry& registry,
    const std::string& prefab_path,
    float x, float y, float z)
{
    auto entity = spawn(registry, prefab_path);
    if (entity == entt::null)
        return entt::null;

    // Override position
    if (auto* t = registry.try_get<TransformComponent>(entity))
        t->position = glm::vec3(x, y, z);

    return entity;
}
