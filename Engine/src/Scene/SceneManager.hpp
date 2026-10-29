#pragma once
#include "EngineExport.h"
#include "Scene.hpp"
#include "world.hpp"
#include <unordered_map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

class IRenderAPI;
class ReflectionRegistry;
class SceneManager;
class SceneOperationHandle {
public:
    SceneOperationHandle() noexcept = default;
    bool valid() const noexcept { return owner_ && incarnation_ && serial_; }
    friend bool operator==(const SceneOperationHandle&, const SceneOperationHandle&) noexcept = default;
private:
    friend class SceneManager;
    const SceneManager* owner_ = nullptr;
    uint64_t incarnation_ = 0, serial_ = 0;
};
enum class SceneOperation { None, Initialize, ConfigureReflection, ConfigureGameplayDefaults, Load, LoadAsync, PumpAsyncLoads, Unload, Clear, Activate, Transition, Shutdown };
enum class ScenePhase { None, CancelWorkers, WaitForRender, ResolveOwnership, Retire, Construct, Rollback, Load, Publish, Complete };
enum class SceneStatus { Completed, CompletedWithObserverFailure, NotFound, WrongThread, NotInitialized, Busy, InvalidArgument, LoadFailure, ConstructionFailure, OwnershipFailure, LifecycleFailure, Partial, InternalFailure, FrameworkInstallFailure };
struct SceneResult {
    SceneStatus status = SceneStatus::Completed;
    SceneOperation operation = SceneOperation::None;
    ScenePhase phase = ScenePhase::None;
    SceneOperationHandle operation_handle{};
    SceneId scene_id = INVALID_SCENE;
    size_t completed_count = 0;
    LifecycleResult lifecycle{};
    LevelInstantiationResult construction{};
    // True when this operation has no pending cleanup to resume. A failed
    // framework install may still leave committed entities owned by its scene;
    // unload or clear retires them.
    bool cleanup_complete = true;
};
class ENGINE_API SceneManager {
public:
    static SceneManager& get() { static SceneManager instance; return instance; }
    SceneManager();
    ~SceneManager() noexcept;
    SceneResult initialize(world*, IRenderAPI*);
    SceneResult setReflectionRegistry(ReflectionRegistry*);
    SceneResult setGameplayDefaults(std::string, std::string);
    SceneResult loadScene(const std::string&);
    SceneResult loadSceneAsync(const std::string&);
    SceneResult pumpAsyncLoads();
    SceneResult unloadScene(SceneId);
    SceneResult clear();
    SceneResult activateScene(SceneId);
    SceneResult transition(const std::string&, TransitionType = TransitionType::Instant);
    SceneResult shutdown();
    SceneResult resume(SceneOperationHandle);
    Scene* getActiveScene();
    const Scene* getActiveScene() const;
    Scene* getScene(SceneId);
    float getLoadProgress(SceneId) const;
    SceneLoadState getSceneState(SceneId) const;
private:
    SceneManager(const SceneManager&) = delete;
    SceneManager& operator=(const SceneManager&) = delete;
    struct AsyncLoadResult { std::mutex mutex; bool finished = false, cancelled = false, success = false; LevelData level_data; };
    struct AsyncLoadTask { std::shared_ptr<AsyncLoadResult> result; std::jthread worker; };
    bool isOwnerThread(const char*) const;
    SceneResult guard(SceneOperation, bool require_initialized = true) const;
    SceneResult start(SceneOperation, SceneId = INVALID_SCENE);
    SceneResult advance();
    SceneResult loadInternal(const std::string&, bool);
    bool unloadStep(SceneId);
    bool activateStep(SceneId);
    SceneResult finish(SceneStatus);
    void cancelAsyncLoad(SceneId);
    void cancelAsyncLoads();
    LevelManager level_manager;
    std::unordered_map<SceneId, Scene> scenes;
    std::unordered_map<SceneId, AsyncLoadTask> async_loads;
    SceneId active_scene_id = INVALID_SCENE;
    uint32_t next_scene_id = 1;
    world* game_world = nullptr;
    IRenderAPI* render_api = nullptr;
    std::thread::id owner_thread = std::this_thread::get_id();
    bool initialized = false;
    uint64_t incarnation = 0, next_serial = 1;
    bool operation_active = false, observer_failure = false, unload_event_attempted = false;
    bool publication_cleanup = false, publication_waiting_rollback = false;
    SceneResult current{};
    LifecycleResult completed_lifecycle{};
    SceneId target_id = INVALID_SCENE;
    std::string target_path;
    world* target_world = nullptr;
    IRenderAPI* target_render = nullptr;
};
