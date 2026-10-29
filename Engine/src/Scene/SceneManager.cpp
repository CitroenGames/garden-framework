#include "SceneManager.hpp"
#include "Graphics/RenderAPI.hpp"
#include "Events/EventBus.hpp"
#include "Events/EngineEvents.hpp"
#include "Utils/Log.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <limits>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace {
std::atomic<bool> fail_scene_publication{false};
std::atomic<uint64_t> next_incarnation{1};
uint64_t allocateIncarnation() noexcept {
    auto value = next_incarnation.load();
    while (value && value != std::numeric_limits<uint64_t>::max())
        if (next_incarnation.compare_exchange_weak(value, value + 1)) return value;
    return 0;
}
}
namespace garden::testing {
ENGINE_API void failNextScenePublicationForTesting() noexcept { fail_scene_publication.store(true); }
}
static_assert(std::is_nothrow_move_constructible_v<SceneResult>);
SceneManager::SceneManager() : incarnation(allocateIncarnation()) {}
SceneManager::~SceneManager() noexcept {
    if (!scenes.empty() || operation_active) {
        // The world and its providers may already be unavailable here. Only
        // report the missing teardown; destruction cannot retire their entities.
        try {
            if (EE::CLog::ShouldLogEngine(spdlog::level::err))
                LOG_ENGINE_ERROR("SceneManager destroyed with incomplete explicit shutdown (scenes={}, operation_pending={})", scenes.size(), operation_active);
            else
                std::fputs("SceneManager destroyed with incomplete explicit shutdown\n", stderr);
        } catch (...) {
            std::fputs("SceneManager destroyed with incomplete explicit shutdown\n", stderr);
        }
    }
    cancelAsyncLoads();
}
bool SceneManager::isOwnerThread(const char*) const { return std::this_thread::get_id() == owner_thread; }
SceneResult SceneManager::guard(SceneOperation op, bool require_initialized) const {
    SceneResult r; r.operation = op;
    if (!isOwnerThread("mutator")) r.status = SceneStatus::WrongThread;
    else if (operation_active) { r = current; r.status = SceneStatus::Busy; }
    else if (require_initialized && !initialized) r.status = SceneStatus::NotInitialized;
    return r;
}
SceneResult SceneManager::start(SceneOperation op, SceneId id) {
    SceneResult r = guard(op, op != SceneOperation::Initialize);
    if (r.status != SceneStatus::Completed) return r;
    if (!incarnation || !next_serial || next_serial == std::numeric_limits<uint64_t>::max()) { r.status = SceneStatus::InternalFailure; return r; }
    current = {}; completed_lifecycle = {}; current.operation = op; current.scene_id = id;
    current.operation_handle.owner_ = this; current.operation_handle.incarnation_ = incarnation; current.operation_handle.serial_ = next_serial++;
    operation_active = true; observer_failure = false; unload_event_attempted = false; publication_cleanup = false; publication_waiting_rollback = false; return current;
}
SceneResult SceneManager::finish(SceneStatus status) {
    current.status = status; current.phase = ScenePhase::Complete; current.cleanup_complete = true;
    if (!current.lifecycle.operation.valid() && completed_lifecycle.operation.valid()) current.lifecycle = completed_lifecycle;
    SceneResult result = std::move(current); operation_active = false; return result;
}
SceneResult SceneManager::initialize(world* w, IRenderAPI* api) {
    auto r = guard(SceneOperation::Initialize, false); if (r.status != SceneStatus::Completed) return r;
    if (!w) { r.status = SceneStatus::InvalidArgument; return r; }
    r = start(SceneOperation::Initialize); if (r.status != SceneStatus::Completed) return r;
    target_world = w; target_render = api; return advance();
}
SceneResult SceneManager::setReflectionRegistry(ReflectionRegistry* p) {
    auto r = guard(SceneOperation::ConfigureReflection); if (r.status == SceneStatus::Completed) level_manager.setReflectionRegistry(p); return r;
}
SceneResult SceneManager::setGameplayDefaults(std::string mode, std::string state) {
    auto r = guard(SceneOperation::ConfigureGameplayDefaults); if (r.status == SceneStatus::Completed) level_manager.setGameplayDefaults(std::move(mode), std::move(state)); return r;
}
SceneResult SceneManager::loadScene(const std::string& path) { auto r = guard(SceneOperation::Load); if (r.status != SceneStatus::Completed) return r; return loadInternal(path, false); }
SceneResult SceneManager::loadSceneAsync(const std::string& path) { auto r = guard(SceneOperation::LoadAsync); if (r.status != SceneStatus::Completed) return r; return loadInternal(path, true); }
SceneResult SceneManager::loadInternal(const std::string& path, bool async) {
    SceneResult r; r.operation = async ? SceneOperation::LoadAsync : SceneOperation::Load;
    if (path.empty() || !next_scene_id) { r.status = SceneStatus::InvalidArgument; return r; }
    try {
        Scene scene; scene.id = next_scene_id++; scene.level_path = path; scene.state = SceneLoadState::Loading;
        r.scene_id = scene.id; scenes.emplace(scene.id, std::move(scene));
        EventBus::get().publish(SceneLoadStartEvent{r.scene_id, path});
        if (!async) {
            auto& s = scenes.at(r.scene_id);
            bool ok = level_manager.loadLevel(path, s.level_data);
            s.state = ok ? SceneLoadState::Loaded : SceneLoadState::Failed; s.load_progress = ok ? 1.0f : 0.0f;
            EventBus::get().publish(SceneLoadCompleteEvent{r.scene_id, path, ok});
            if (!ok) r.status = SceneStatus::LoadFailure;
        } else {
            auto result = std::make_shared<AsyncLoadResult>(); AsyncLoadTask task; task.result = result;
            task.worker = std::jthread([result, path](std::stop_token stop) {
                LevelData data; bool ok = false;
                try { LevelManager loader; ok = loader.loadLevel(path, data); } catch (...) {}
                std::lock_guard lock(result->mutex); result->cancelled = stop.stop_requested(); result->success = ok && !result->cancelled;
                if (result->success) result->level_data = std::move(data); result->finished = true;
            });
            async_loads.emplace(r.scene_id, std::move(task));
        }
    } catch (...) { if (auto it = scenes.find(r.scene_id); it != scenes.end()) it->second.state = SceneLoadState::Failed; r.status = SceneStatus::InternalFailure; }
    return r;
}
SceneResult SceneManager::pumpAsyncLoads() {
    auto r = guard(SceneOperation::PumpAsyncLoads); if (r.status != SceneStatus::Completed) return r;
    for (auto it = async_loads.begin(); it != async_loads.end();) {
        auto id = it->first; auto data = it->second.result; bool done, cancelled, success; LevelData level;
        { std::lock_guard lock(data->mutex); done = data->finished; cancelled = data->cancelled; success = data->success; if (done && success) level = std::move(data->level_data); }
        if (!done) { ++it; continue; } it = async_loads.erase(it); ++r.completed_count;
        auto s = scenes.find(id); if (s == scenes.end() || cancelled) continue;
        s->second.state = success ? SceneLoadState::Loaded : SceneLoadState::Failed;
        if (success) { s->second.level_data = std::move(level); s->second.load_progress = 1.0f; }
        try { EventBus::get().publish(SceneLoadCompleteEvent{id, s->second.level_path, success}); } catch (...) { r.status = SceneStatus::CompletedWithObserverFailure; }
    } return r;
}
void SceneManager::cancelAsyncLoad(SceneId id) { auto it = async_loads.find(id); if (it != async_loads.end()) { it->second.worker.request_stop(); async_loads.erase(it); } }
void SceneManager::cancelAsyncLoads() { for (auto& [id, task] : async_loads) task.worker.request_stop(); async_loads.clear(); }
SceneResult SceneManager::unloadScene(SceneId id) {
    auto r = guard(SceneOperation::Unload); if (r.status != SceneStatus::Completed) return r;
    if (!scenes.contains(id)) { r.status = SceneStatus::NotFound; return r; }
    r = start(SceneOperation::Unload, id); if (r.status != SceneStatus::Completed) return r; target_id = id; return advance();
}
SceneResult SceneManager::clear() { auto r = start(SceneOperation::Clear); return r.status == SceneStatus::Completed ? advance() : r; }
SceneResult SceneManager::shutdown() { auto r = start(SceneOperation::Shutdown); return r.status == SceneStatus::Completed ? advance() : r; }
SceneResult SceneManager::activateScene(SceneId id) {
    auto r = guard(SceneOperation::Activate); if (r.status != SceneStatus::Completed) return r;
    auto it = scenes.find(id); if (it == scenes.end()) { r.status = SceneStatus::NotFound; return r; }
    if (it->second.state != SceneLoadState::Loaded) { r.status = SceneStatus::InvalidArgument; return r; }
    r = start(SceneOperation::Activate, id); if (r.status != SceneStatus::Completed) return r; target_id = id; return advance();
}
SceneResult SceneManager::transition(const std::string& path, TransitionType type) {
    auto r = guard(SceneOperation::Transition); if (r.status != SceneStatus::Completed) return r;
    if (path.empty()) { r.status = SceneStatus::InvalidArgument; return r; }
    (void)type; try { target_path = path; } catch (...) { r.status = SceneStatus::InternalFailure; return r; }
    r = start(SceneOperation::Transition); if (r.status != SceneStatus::Completed) return r; target_id = INVALID_SCENE; return advance();
}
SceneResult SceneManager::resume(SceneOperationHandle h) {
    SceneResult r; if (!isOwnerThread("resume")) { r.status = SceneStatus::WrongThread; return r; }
    if (!operation_active || !h.valid() || !(h == current.operation_handle)) { r.status = SceneStatus::InvalidArgument; return r; }
    return advance();
}
bool SceneManager::unloadStep(SceneId id) {
    auto it = scenes.find(id); if (it == scenes.end()) return true;
    auto& s = it->second; current.scene_id = id;
    if (!current.lifecycle.operation.valid()) {
        current.phase = ScenePhase::CancelWorkers; cancelAsyncLoad(id);
        current.phase = ScenePhase::WaitForRender; if (render_api) render_api->waitForGPU();
        current.phase = ScenePhase::ResolveOwnership;
        std::vector<garden::ecs::EntityHandle> roots;
        try {
            for (auto h : s.owned_entities) {
                auto c = game_world->classifyEntityHandle(h);
                if (c == EntityHandleClassification::RetiredGone) continue;
                if (c == EntityHandleClassification::Retiring) {
                    current.lifecycle.status = LifecycleStatus::Conflict;
                    current.status = SceneStatus::LifecycleFailure; return false;
                }
                if (c != EntityHandleClassification::Alive && c != EntityHandleClassification::Pending) { current.status = SceneStatus::OwnershipFailure; return false; }
                if (std::find(roots.begin(), roots.end(), h) == roots.end()) roots.push_back(h);
            }
        } catch (...) { current.status = SceneStatus::InternalFailure; return false; }
        current.phase = ScenePhase::Retire; current.lifecycle = game_world->retireEntities(roots);
    } else current.lifecycle = game_world->resumeRetirement(current.lifecycle.operation);
    if (!current.lifecycle.cleanupComplete()) { current.status = SceneStatus::LifecycleFailure; current.cleanup_complete = current.lifecycle.operation.valid() ? false : true; return false; }
    if (current.lifecycle.status == LifecycleStatus::CompletedWithObserverFailure) observer_failure = true;
    if (current.lifecycle.operation.valid()) {
        auto ack = game_world->acknowledgeRetirement(current.lifecycle.operation);
        if (!ack.cleanupComplete()) { current.lifecycle = ack; current.status = SceneStatus::LifecycleFailure; current.cleanup_complete = false; return false; }
    }
    completed_lifecycle = current.lifecycle; current.lifecycle = {}; current.cleanup_complete = true;
    if (active_scene_id == id) active_scene_id = INVALID_SCENE;
    current.phase = ScenePhase::Publish;
    if (!unload_event_attempted) { unload_event_attempted = true; try { EventBus::get().publish(SceneUnloadEvent{id, s.level_path}); } catch (...) { observer_failure = true; } }
    scenes.erase(it); unload_event_attempted = false; return true;
}
bool SceneManager::activateStep(SceneId id) {
    auto it = scenes.find(id); if (it == scenes.end()) { current.status = SceneStatus::NotFound; return false; }
    auto& s = it->second; current.scene_id = id;
    if (current.phase != ScenePhase::Rollback) {
        current.phase = ScenePhase::Construct;
        current.construction = level_manager.instantiateLevelParallel(s.level_data, *game_world, render_api);
        if (current.construction.status == LevelInstantiationStatus::Completed ||
            current.construction.status == LevelInstantiationStatus::FrameworkInstallFailure) {
            // LevelManager has committed this cohort. Keep its original handles in
            // the operation while all potentially throwing scene records are staged.
            // Nothing is published to the scene until every allocation succeeds.
            try {
                if (current.construction.rollback.operation.valid()) {
                    // A successful result must not contain a rollback token.
                    // Fail closed and settle the token before cohort cleanup.
                    current.lifecycle = current.construction.rollback;
                    publication_waiting_rollback = true;
                    throw std::logic_error("successful construction returned a rollback token");
                }
                if (fail_scene_publication.exchange(false)) throw std::bad_alloc();
                auto owned = current.construction.entities;
                std::vector<EntityRef> references;
                references.reserve(owned.size());
                for (auto h : owned) references.push_back(game_world->persistedReference(h));
                const auto player_ref = game_world->persistedReference(current.construction.player);
                const auto freecam_ref = game_world->persistedReference(current.construction.freecam);
                const auto player_rep_ref = game_world->persistedReference(current.construction.player_rep);

                s.owned_entities = std::move(owned);
                s.owned_references = std::move(references);
                s.player_entity = current.construction.player;
                s.freecam_entity = current.construction.freecam;
                s.player_rep_entity = current.construction.player_rep;
                s.player_reference = player_ref;
                s.freecam_reference = freecam_ref;
                s.player_rep_reference = player_rep_ref;
                if (current.construction.status == LevelInstantiationStatus::FrameworkInstallFailure) {
                    // The cohort is committed and belongs to this scene even
                    // though framework effects may be only partially installed.
                    // Do not retry activation or publish a successful load.
                    s.state = SceneLoadState::Failed;
                    current.status = SceneStatus::FrameworkInstallFailure;
                    return false;
                }
                s.state = SceneLoadState::Active;
                active_scene_id = id;
                current.phase = ScenePhase::Publish;
                try { EventBus::get().publish(LevelLoadedEvent{s.level_data.metadata.level_name, s.level_path}); }
                catch (...) { observer_failure = true; }
                return true;
            } catch (const std::bad_alloc&) {
                current.construction.status = LevelInstantiationStatus::AllocationFailure;
            } catch (...) {
                current.construction.status = LevelInstantiationStatus::ConstructionFailure;
            }
            // The cohort was committed by LevelManager, so retire it as a
            // published batch. The original handles remain in construction even
            // when preflight cannot yet accept a token; resume retries preflight.
            current.construction.player = {};
            current.construction.freecam = {};
            current.construction.player_rep = {};
            if (!publication_waiting_rollback) current.lifecycle = {};
            publication_cleanup = true;
        } else {
            current.lifecycle = current.construction.rollback;
        }
        current.phase = ScenePhase::Rollback;
    }

    if (publication_waiting_rollback) {
        if (!current.lifecycle.cleanupComplete())
            current.lifecycle = game_world->resumeRetirement(current.lifecycle.operation);
        current.construction.rollback = current.lifecycle;
        if (!current.lifecycle.cleanupComplete()) {
            current.status = SceneStatus::LifecycleFailure; current.cleanup_complete = false; return false;
        }
        auto ack = game_world->acknowledgeRetirement(current.lifecycle.operation);
        if (!ack.cleanupComplete()) {
            current.lifecycle = ack; current.construction.rollback = ack;
            current.status = SceneStatus::LifecycleFailure; current.cleanup_complete = false; return false;
        }
        publication_waiting_rollback = false;
        current.lifecycle = {};
    }
    if (publication_cleanup && !current.lifecycle.operation.valid()) {
        try {
            std::vector<garden::ecs::EntityHandle> roots;
            roots.reserve(current.construction.entities.size());
            for (auto h : current.construction.entities) {
                auto classification = game_world->classifyEntityHandle(h);
                if (classification == EntityHandleClassification::RetiredGone) continue;
                if (classification != EntityHandleClassification::Alive &&
                    classification != EntityHandleClassification::Pending) {
                    current.status = SceneStatus::OwnershipFailure;
                    current.cleanup_complete = false; return false;
                }
                if (std::find(roots.begin(), roots.end(), h) == roots.end()) roots.push_back(h);
            }
            current.lifecycle = game_world->retireEntities(roots);
        } catch (...) {
            current.status = SceneStatus::Partial; current.cleanup_complete = false; return false;
        }
    } else if (current.lifecycle.operation.valid() && !current.lifecycle.cleanupComplete()) {
        current.lifecycle = game_world->resumeRetirement(current.lifecycle.operation);
    }
    current.construction.rollback = current.lifecycle;
    if (!current.lifecycle.cleanupComplete()) {
        current.status = SceneStatus::LifecycleFailure;
        // A rejected published batch has no token, but this operation still
        // owns the cohort and must retry instead of dropping its identities.
        current.cleanup_complete = !publication_cleanup && !current.lifecycle.operation.valid();
        return false;
    }
    if (current.lifecycle.operation.valid()) {
        auto ack = game_world->acknowledgeRetirement(current.lifecycle.operation);
        if (!ack.cleanupComplete()) { current.lifecycle = ack; current.construction.rollback = ack; current.status = SceneStatus::LifecycleFailure; current.cleanup_complete = false; return false; }
    }
    current.cleanup_complete = true; current.status = SceneStatus::ConstructionFailure;
    s.state = SceneLoadState::Loaded;
    return false;
}
SceneResult SceneManager::advance() {
    try {
        current.status = SceneStatus::Completed;
        auto op = current.operation;
        if (op == SceneOperation::Unload) {
            if (!unloadStep(target_id)) { if (current.cleanup_complete) return finish(current.status); return current; }
        } else if (op == SceneOperation::Clear || op == SceneOperation::Shutdown || op == SceneOperation::Initialize) {
            while (!scenes.empty()) {
                SceneId id = std::min_element(scenes.begin(), scenes.end(), [](const auto& a, const auto& b) { return a.first < b.first; })->first;
                if (!unloadStep(id)) { if (current.cleanup_complete) return finish(current.status); return current; }
            }
            if (op == SceneOperation::Initialize) { game_world = target_world; render_api = target_render; initialized = true; }
            if (op == SceneOperation::Shutdown) { initialized = false; game_world = nullptr; render_api = nullptr; }
        } else if (op == SceneOperation::Activate || op == SceneOperation::Transition) {
            if (active_scene_id != INVALID_SCENE && active_scene_id != target_id) {
                if (!unloadStep(active_scene_id)) { if (current.cleanup_complete) return finish(current.status); return current; }
            }
            if (op == SceneOperation::Transition && target_id == INVALID_SCENE) {
                current.phase = ScenePhase::Load; auto loaded = loadInternal(target_path, false); target_id = loaded.scene_id; current.scene_id = target_id;
                if (loaded.status != SceneStatus::Completed) return finish(loaded.status);
            }
            if (!activateStep(target_id)) { if (current.cleanup_complete) return finish(current.status); return current; }
        }
        return finish(observer_failure ? SceneStatus::CompletedWithObserverFailure : SceneStatus::Completed);
    } catch (...) { if (!current.cleanup_complete || current.lifecycle.operation.valid()) { current.status = SceneStatus::Partial; current.cleanup_complete = false; return current; } return finish(SceneStatus::InternalFailure); }
}
Scene* SceneManager::getActiveScene() { return isOwnerThread("getActiveScene") ? getScene(active_scene_id) : nullptr; }
const Scene* SceneManager::getActiveScene() const { if (!isOwnerThread("getActiveScene")) return nullptr; auto it = scenes.find(active_scene_id); return it == scenes.end() ? nullptr : &it->second; }
Scene* SceneManager::getScene(SceneId id) { if (!isOwnerThread("getScene")) return nullptr; auto it = scenes.find(id); return it == scenes.end() ? nullptr : &it->second; }
float SceneManager::getLoadProgress(SceneId id) const { auto it = scenes.find(id); return isOwnerThread("getLoadProgress") && it != scenes.end() ? it->second.load_progress : 0.0f; }
SceneLoadState SceneManager::getSceneState(SceneId id) const { auto it = scenes.find(id); return isOwnerThread("getSceneState") && it != scenes.end() ? it->second.state : SceneLoadState::Unloaded; }
