#pragma once

#include "EngineExport.h"
#include "Scene.hpp"
#include <unordered_map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

class world;
class IRenderAPI;
class ReflectionRegistry;

class ENGINE_API SceneManager
{
public:
    static SceneManager& get()
    {
        static SceneManager instance;
        return instance;
    }

    // Initialize with references to world and render API
    void initialize(world* game_world, IRenderAPI* render_api);
    void setReflectionRegistry(ReflectionRegistry* reflection);
    void setGameplayDefaults(std::string game_mode_class, std::string game_state_class);

    // Load a scene from a level file synchronously.
    SceneId loadScene(const std::string& level_path);

    // Parse level data on a worker. Call pumpAsyncLoads() on the initializing
    // thread to publish completion and make the scene activatable.
    SceneId loadSceneAsync(const std::string& level_path);
    size_t pumpAsyncLoads();

    // Unload a scene - destroys its entities and cleans up physics
    void unloadScene(SceneId id);

    // Activate a loaded scene (makes it current, instantiates entities)
    bool activateScene(SceneId id);

    // Transition to a new scene (load + activate + unload old)
    bool transition(const std::string& level_path, TransitionType type = TransitionType::Instant);

    // Get scene info
    Scene* getActiveScene();
    const Scene* getActiveScene() const;
    Scene* getScene(SceneId id);
    float getLoadProgress(SceneId id) const;
    SceneLoadState getSceneState(SceneId id) const;

    // Cleanup all scenes
    void clear();

    SceneManager() = default;
    ~SceneManager();

private:
    SceneManager(const SceneManager&) = delete;
    SceneManager& operator=(const SceneManager&) = delete;

    void unloadSceneEntities(Scene& scene);
    bool isOwnerThread(const char* operation) const;
    void cancelAsyncLoad(SceneId id);
    void cancelAsyncLoads();

    struct AsyncLoadResult
    {
        std::mutex mutex;
        bool finished = false;
        bool cancelled = false;
        bool success = false;
        LevelData level_data;
    };

    struct AsyncLoadTask
    {
        std::shared_ptr<AsyncLoadResult> result;
        std::jthread worker;
    };

    LevelManager level_manager;
    std::unordered_map<SceneId, Scene> scenes;
    std::unordered_map<SceneId, AsyncLoadTask> async_loads;
    SceneId active_scene_id = INVALID_SCENE;
    uint32_t next_scene_id = 1;

    world* game_world = nullptr;
    IRenderAPI* render_api = nullptr;
    std::thread::id owner_thread = std::this_thread::get_id();
    bool initialized = false;
};
