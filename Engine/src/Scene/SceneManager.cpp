#include "SceneManager.hpp"
#include "world.hpp"
#include "Components/Components.hpp"
#include "Graphics/RenderAPI.hpp"
#include "Events/EventBus.hpp"
#include "Events/EngineEvents.hpp"
#include "Utils/Log.hpp"

#include <exception>
#include <unordered_set>
#include <utility>

SceneManager::~SceneManager()
{
    // Workers never capture this, world, or render_api. Joining here makes
    // manager destruction safe even when a level parse is still in flight.
    cancelAsyncLoads();
}

bool SceneManager::isOwnerThread(const char* operation) const
{
    if (std::this_thread::get_id() == owner_thread)
        return true;

    LOG_ENGINE_ERROR("SceneManager::{} must run on the initializing thread", operation);
    return false;
}

void SceneManager::initialize(world* w, IRenderAPI* api)
{
    if (initialized && !isOwnerThread("initialize"))
        return;

    if (initialized && (game_world != w || render_api != api))
        clear();

    owner_thread = std::this_thread::get_id();
    game_world = w;
    render_api = api;
    initialized = true;
}

void SceneManager::setReflectionRegistry(ReflectionRegistry* reflection)
{
    if (isOwnerThread("setReflectionRegistry"))
        level_manager.setReflectionRegistry(reflection);
}

void SceneManager::setGameplayDefaults(std::string game_mode_class, std::string game_state_class)
{
    if (isOwnerThread("setGameplayDefaults"))
        level_manager.setGameplayDefaults(std::move(game_mode_class), std::move(game_state_class));
}

SceneId SceneManager::loadScene(const std::string& level_path)
{
    if (!isOwnerThread("loadScene"))
        return INVALID_SCENE;

    const SceneId id = next_scene_id++;

    Scene scene;
    scene.id = id;
    scene.level_path = level_path;
    scene.state = SceneLoadState::Loading;
    scenes[id] = std::move(scene);

    EventBus::get().publish(SceneLoadStartEvent{id, level_path});

    Scene& stored_scene = scenes.at(id);
    if (!level_manager.loadLevel(level_path, stored_scene.level_data))
    {
        LOG_ENGINE_ERROR("SceneManager: failed to load level '{}'", level_path);
        stored_scene.state = SceneLoadState::Failed;
        EventBus::get().publish(SceneLoadCompleteEvent{id, level_path, false});
        return id;
    }

    stored_scene.state = SceneLoadState::Loaded;
    stored_scene.load_progress = 1.0f;
    EventBus::get().publish(SceneLoadCompleteEvent{id, level_path, true});
    LOG_ENGINE_INFO("SceneManager: loaded scene '{}' (id={})", level_path, id);
    return id;
}

SceneId SceneManager::loadSceneAsync(const std::string& level_path)
{
    if (!isOwnerThread("loadSceneAsync"))
        return INVALID_SCENE;

    const SceneId id = next_scene_id++;
    Scene scene;
    scene.id = id;
    scene.level_path = level_path;
    scene.state = SceneLoadState::Loading;
    scenes[id] = std::move(scene);
    EventBus::get().publish(SceneLoadStartEvent{id, level_path});

    auto result = std::make_shared<AsyncLoadResult>();
    AsyncLoadTask task;
    task.result = result;

    try
    {
        task.worker = std::jthread([result, level_path](std::stop_token stop_token)
        {
            bool success = false;
            LevelData parsed_level;
            try
            {
                // This loader is task-local. It reads and parses files only;
                // ECS instantiation and GPU uploads remain in activateScene.
                LevelManager loader;
                success = loader.loadLevel(level_path, parsed_level);
            }
            catch (const std::exception& e)
            {
                LOG_ENGINE_ERROR("SceneManager: exception loading '{}': {}", level_path, e.what());
            }
            catch (...)
            {
                LOG_ENGINE_ERROR("SceneManager: unknown exception loading '{}'", level_path);
            }

            std::lock_guard<std::mutex> lock(result->mutex);
            result->cancelled = stop_token.stop_requested();
            result->success = success && !result->cancelled;
            if (result->success)
                result->level_data = std::move(parsed_level);
            result->finished = true;
        });
    }
    catch (const std::exception& e)
    {
        Scene& failed_scene = scenes.at(id);
        failed_scene.state = SceneLoadState::Failed;
        LOG_ENGINE_ERROR("SceneManager: could not start loader for '{}': {}", level_path, e.what());
        EventBus::get().publish(SceneLoadCompleteEvent{id, level_path, false});
        return id;
    }

    async_loads.emplace(id, std::move(task));
    return id;
}

size_t SceneManager::pumpAsyncLoads()
{
    if (!isOwnerThread("pumpAsyncLoads"))
        return 0;

    size_t completed_count = 0;
    for (auto it = async_loads.begin(); it != async_loads.end();)
    {
        const SceneId id = it->first;
        const auto result = it->second.result;
        bool finished = false;
        bool cancelled = false;
        bool success = false;
        LevelData parsed_level;
        {
            std::lock_guard<std::mutex> lock(result->mutex);
            finished = result->finished;
            if (finished)
            {
                cancelled = result->cancelled;
                success = result->success;
                if (success)
                    parsed_level = std::move(result->level_data);
            }
        }

        if (!finished)
        {
            ++it;
            continue;
        }

        // Erasing joins the already-completed jthread before releasing its
        // task state. The worker has no reference to this manager.
        it = async_loads.erase(it);
        ++completed_count;

        auto scene_it = scenes.find(id);
        if (scene_it == scenes.end() || cancelled)
            continue;

        Scene& scene = scene_it->second;
        if (success)
        {
            scene.level_data = std::move(parsed_level);
            scene.state = SceneLoadState::Loaded;
            scene.load_progress = 1.0f;
        }
        else
        {
            scene.state = SceneLoadState::Failed;
        }

        EventBus::get().publish(SceneLoadCompleteEvent{id, scene.level_path, success});
        if (success)
            LOG_ENGINE_INFO("SceneManager: asynchronously loaded '{}' (id={})", scene.level_path, id);
        else
            LOG_ENGINE_ERROR("SceneManager: failed to load scene '{}'", scene.level_path);
    }
    return completed_count;
}

void SceneManager::cancelAsyncLoad(SceneId id)
{
    auto it = async_loads.find(id);
    if (it == async_loads.end())
        return;
    it->second.worker.request_stop();
    async_loads.erase(it); // jthread joins before the result state is released
}

void SceneManager::cancelAsyncLoads()
{
    for (auto& [id, task] : async_loads)
    {
        (void)id;
        task.worker.request_stop();
    }
    async_loads.clear(); // jthread destructors join
}

void SceneManager::unloadScene(SceneId id)
{
    if (!isOwnerThread("unloadScene"))
        return;

    if (render_api)
        render_api->waitForGPU();

    cancelAsyncLoad(id);
    auto it = scenes.find(id);
    if (it == scenes.end())
        return;

    Scene& scene = it->second;
    LOG_ENGINE_INFO("SceneManager: unloading scene '{}' (id={})", scene.level_path, id);
    unloadSceneEntities(scene);

    if (active_scene_id == id)
        active_scene_id = INVALID_SCENE;

    EventBus::get().publish(SceneUnloadEvent{id, scene.level_path});
    scenes.erase(it);
}

bool SceneManager::activateScene(SceneId id)
{
    if (!isOwnerThread("activateScene"))
        return false;

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

    if (active_scene_id != INVALID_SCENE && active_scene_id != id)
        unloadScene(active_scene_id);

    Scene& scene = scenes.at(id);
    entt::entity player = entt::null;
    entt::entity freecam = entt::null;
    entt::entity player_rep = entt::null;
    std::vector<entt::entity> created_entities;

    const bool instantiated = level_manager.instantiateLevelParallel(
        scene.level_data, *game_world, render_api,
        &player, &freecam, &player_rep, true, &created_entities);

    // Record the exact output from LevelManager, including partial output if
    // instantiation reports failure, so cleanup cannot sweep other entities.
    scene.owned_entities.clear();
    scene.owned_entities.reserve(created_entities.size());
    for (entt::entity entity : created_entities)
    {
        if (game_world->registry.valid(entity))
            scene.owned_entities.push_back(ensureEntityRef(game_world->registry, entity));
    }

    if (!instantiated)
    {
        LOG_ENGINE_ERROR("SceneManager: failed to instantiate scene '{}'", scene.level_path);
        unloadSceneEntities(scene);
        scene.state = SceneLoadState::Loaded;
        return false;
    }

    scene.player_entity = entityRefOf(game_world->registry, player);
    scene.freecam_entity = entityRefOf(game_world->registry, freecam);
    scene.player_rep_entity = entityRefOf(game_world->registry, player_rep);
    scene.state = SceneLoadState::Active;
    active_scene_id = id;

    EventBus::get().publish(LevelLoadedEvent{
        scene.level_data.metadata.level_name,
        scene.level_path
    });

    LOG_ENGINE_INFO("SceneManager: activated scene '{}' (id={})", scene.level_path, id);
    return true;
}

bool SceneManager::transition(const std::string& level_path, TransitionType type)
{
    (void)type;
    if (!isOwnerThread("transition"))
        return false;

    if (render_api)
        render_api->waitForGPU();

    if (active_scene_id != INVALID_SCENE)
        unloadScene(active_scene_id);

    // Scene teardown is ownership-scoped. Do not clear unrelated world
    // entities or reinitialize Jolt as a substitute for removing their bodies.
    const SceneId new_id = loadScene(level_path);
    if (getSceneState(new_id) == SceneLoadState::Failed)
        return false;

    return activateScene(new_id);
}

Scene* SceneManager::getActiveScene()
{
    if (!isOwnerThread("getActiveScene")) return nullptr;
    if (active_scene_id == INVALID_SCENE) return nullptr;
    auto it = scenes.find(active_scene_id);
    return it != scenes.end() ? &it->second : nullptr;
}

const Scene* SceneManager::getActiveScene() const
{
    if (!isOwnerThread("getActiveScene")) return nullptr;
    if (active_scene_id == INVALID_SCENE) return nullptr;
    auto it = scenes.find(active_scene_id);
    return it != scenes.end() ? &it->second : nullptr;
}

Scene* SceneManager::getScene(SceneId id)
{
    if (!isOwnerThread("getScene")) return nullptr;
    auto it = scenes.find(id);
    return it != scenes.end() ? &it->second : nullptr;
}

float SceneManager::getLoadProgress(SceneId id) const
{
    if (!isOwnerThread("getLoadProgress")) return 0.0f;
    auto it = scenes.find(id);
    return it != scenes.end() ? it->second.load_progress : 0.0f;
}

SceneLoadState SceneManager::getSceneState(SceneId id) const
{
    if (!isOwnerThread("getSceneState")) return SceneLoadState::Unloaded;
    auto it = scenes.find(id);
    return it != scenes.end() ? it->second.state : SceneLoadState::Unloaded;
}

void SceneManager::clear()
{
    if (!isOwnerThread("clear"))
        return;

    cancelAsyncLoads();
    if (render_api)
        render_api->waitForGPU();

    for (auto& [id, scene] : scenes)
    {
        (void)id;
        unloadSceneEntities(scene);
    }
    scenes.clear();
    active_scene_id = INVALID_SCENE;
}

void SceneManager::unloadSceneEntities(Scene& scene)
{
    if (!game_world)
        return;

    auto& registry = game_world->registry;
    std::vector<entt::entity> owned_entities;
    owned_entities.reserve(scene.owned_entities.size());
    for (EntityRef owned_ref : scene.owned_entities)
    {
        const entt::entity entity = resolveEntityRef(registry, owned_ref);
        if (!registry.valid(entity))
            continue;
        owned_entities.push_back(entity);
    }

    // Level constraints are keyed by their source entity. Remove every owned
    // constraint before any target body can be destroyed, regardless of the
    // order in which entities appeared in the level file.
    auto& physics = game_world->getPhysicsSystem();
    for (entt::entity entity : owned_entities)
        physics.removeConstraint(entity);

    std::unordered_set<EntityGuid, GuidHash> owned_guids;
    owned_guids.reserve(scene.owned_entities.size());
    for (EntityRef owned_ref : scene.owned_entities)
    {
        if (!owned_ref.isNull())
            owned_guids.insert(owned_ref.guid);
    }

    // A constraint can also be owned by an unrelated entity while targeting
    // a scene-owned body. ConstraintComponent stores that relationship as a
    // stable ref, so remove the physics link before destroying the target.
    for (auto [entity, constraint] : registry.view<ConstraintComponent>().each())
    {
        if (owned_guids.contains(constraint.target_entity.guid))
            physics.removeConstraint(entity);
    }

    for (entt::entity entity : owned_entities)
        physics.removeBody(entity);

    for (entt::entity entity : owned_entities)
    {
        registry.destroy(entity);
    }

    scene.owned_entities.clear();
    scene.player_entity = EntityRef{};
    scene.freecam_entity = EntityRef{};
    scene.player_rep_entity = EntityRef{};
    if (scene.state == SceneLoadState::Active)
        scene.state = SceneLoadState::Unloaded;
}
