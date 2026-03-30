#include "SceneManager.hpp"
#include "world.hpp"
#include "Graphics/RenderAPI.hpp"
#include "Events/EventBus.hpp"
#include "Events/EngineEvents.hpp"
#include "Utils/Log.hpp"

void SceneManager::initialize(world* w, IRenderAPI* api)
{
    game_world = w;
    render_api = api;
}

SceneId SceneManager::loadScene(const std::string& level_path)
{
    SceneId id = next_scene_id++;

    Scene scene;
    scene.id = id;
    scene.level_path = level_path;
    scene.state = SceneLoadState::Loading;

    EventBus::get().publish(SceneLoadStartEvent{id, level_path});

    if (!level_manager.loadLevel(level_path, scene.level_data))
    {
        LOG_ENGINE_ERROR("SceneManager: failed to load level '{}'", level_path);
        scene.state = SceneLoadState::Failed;
        scenes[id] = std::move(scene);
        return id;
    }

    scene.state = SceneLoadState::Loaded;
    scene.load_progress = 1.0f;
    scenes[id] = std::move(scene);

    EventBus::get().publish(SceneLoadCompleteEvent{id, level_path, true});
    LOG_ENGINE_INFO("SceneManager: loaded scene '{}' (id={})", level_path, id);
    return id;
}

void SceneManager::unloadScene(SceneId id)
{
    auto it = scenes.find(id);
    if (it == scenes.end()) return;

    Scene& scene = it->second;
    LOG_ENGINE_INFO("SceneManager: unloading scene '{}' (id={})", scene.level_path, id);

    unloadSceneEntities(scene);

    if (active_scene_id == id)
    {
        active_scene_id = INVALID_SCENE;
    }

    EventBus::get().publish(SceneUnloadEvent{id, scene.level_path});
    scenes.erase(it);
}

bool SceneManager::activateScene(SceneId id)
{
    auto it = scenes.find(id);
    if (it == scenes.end() || it->second.state != SceneLoadState::Loaded)
    {
        LOG_ENGINE_ERROR("SceneManager: cannot activate scene {} (not loaded)", id);
        return false;
    }

    if (!game_world || !render_api)
    {
        LOG_ENGINE_ERROR("SceneManager: not initialized (world/render_api null)");
        return false;
    }

    Scene& scene = it->second;

    // Instantiate entities
    entt::entity player = entt::null;
    entt::entity freecam = entt::null;
    entt::entity player_rep = entt::null;

    if (!level_manager.instantiateLevel(scene.level_data, *game_world, render_api,
                                         &player, &freecam, &player_rep))
    {
        LOG_ENGINE_ERROR("SceneManager: failed to instantiate scene '{}'", scene.level_path);
        return false;
    }

    scene.player_entity = player;
    scene.freecam_entity = freecam;
    scene.player_rep_entity = player_rep;
    scene.state = SceneLoadState::Active;
    active_scene_id = id;

    // Track owned entities (all entities in registry that were just created)
    // Note: this is approximate - in a full implementation we'd track entity creation
    // during instantiateLevel

    EventBus::get().publish(LevelLoadedEvent{
        scene.level_data.metadata.level_name,
        scene.level_path
    });

    LOG_ENGINE_INFO("SceneManager: activated scene '{}' (id={})", scene.level_path, id);
    return true;
}

bool SceneManager::transition(const std::string& level_path, TransitionType type)
{
    // Unload current scene if one exists
    if (active_scene_id != INVALID_SCENE)
    {
        unloadScene(active_scene_id);
    }

    // Clear the world registry and reinitialize physics for a clean slate
    if (game_world)
    {
        game_world->registry.clear();
        game_world->initializePhysics();
    }

    // Load and activate new scene
    SceneId new_id = loadScene(level_path);
    if (getSceneState(new_id) == SceneLoadState::Failed)
    {
        return false;
    }

    return activateScene(new_id);
}

Scene* SceneManager::getActiveScene()
{
    if (active_scene_id == INVALID_SCENE) return nullptr;
    auto it = scenes.find(active_scene_id);
    return it != scenes.end() ? &it->second : nullptr;
}

const Scene* SceneManager::getActiveScene() const
{
    if (active_scene_id == INVALID_SCENE) return nullptr;
    auto it = scenes.find(active_scene_id);
    return it != scenes.end() ? &it->second : nullptr;
}

Scene* SceneManager::getScene(SceneId id)
{
    auto it = scenes.find(id);
    return it != scenes.end() ? &it->second : nullptr;
}

float SceneManager::getLoadProgress(SceneId id) const
{
    auto it = scenes.find(id);
    return it != scenes.end() ? it->second.load_progress : 0.0f;
}

SceneLoadState SceneManager::getSceneState(SceneId id) const
{
    auto it = scenes.find(id);
    return it != scenes.end() ? it->second.state : SceneLoadState::Unloaded;
}

void SceneManager::clear()
{
    // Unload all scenes
    for (auto& [id, scene] : scenes)
    {
        if (scene.state == SceneLoadState::Active)
        {
            unloadSceneEntities(scene);
        }
    }
    scenes.clear();
    active_scene_id = INVALID_SCENE;
}

void SceneManager::unloadSceneEntities(Scene& scene)
{
    if (!game_world) return;

    // Destroy tracked entities
    for (auto entity : scene.owned_entities)
    {
        if (game_world->registry.valid(entity))
        {
            game_world->registry.destroy(entity);
        }
    }
    scene.owned_entities.clear();
    scene.player_entity = entt::null;
    scene.freecam_entity = entt::null;
    scene.player_rep_entity = entt::null;
    scene.state = SceneLoadState::Unloaded;
}
