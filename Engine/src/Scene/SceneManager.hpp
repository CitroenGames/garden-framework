#pragma once

#include "Scene.hpp"
#include <unordered_map>
#include <memory>
#include <functional>

class world;
class IRenderAPI;

class SceneManager
{
public:
    static SceneManager& get()
    {
        static SceneManager instance;
        return instance;
    }

    // Initialize with references to world and render API
    void initialize(world* game_world, IRenderAPI* render_api);

    // Load a scene from a level file (synchronous for now)
    SceneId loadScene(const std::string& level_path);

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

private:
    SceneManager() = default;
    ~SceneManager() = default;
    SceneManager(const SceneManager&) = delete;
    SceneManager& operator=(const SceneManager&) = delete;

    void unloadSceneEntities(Scene& scene);

    LevelManager level_manager;
    std::unordered_map<SceneId, Scene> scenes;
    SceneId active_scene_id = INVALID_SCENE;
    uint32_t next_scene_id = 1;

    world* game_world = nullptr;
    IRenderAPI* render_api = nullptr;
};
