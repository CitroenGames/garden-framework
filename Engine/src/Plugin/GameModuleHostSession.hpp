#pragma once

#include "EngineExport.h"
#include "Plugin/GameModuleLoader.hpp"

#include <cstddef>
#include <functional>
#include <string>

class ReflectionRegistry;

// GameModuleHostSession is the host-owned lifecycle boundary for one game
// module image.  It coordinates callback order and host cleanup; it does not
// contain game state or game rules, which remain in the template's ECS world.
class ENGINE_API GameModuleHostSession final
{
public:
    enum class Phase
    {
        Idle,
        Registered,
        ClientInitialised,
        ServerInitialised,
        Ready,
        InLevel,
        Playing,
        Quiescing,
        Teardown,
    };

    // A dedicated or listen-server host supplies this only at a scheduler
    // boundary where no module callback is in flight.  The session rejects a
    // server-role reload when the host cannot prove this state, or while an
    // authoritative client remains connected.  Network ownership stays with
    // the host; the generic module session never drains or disconnects peers.
    struct ServerReloadState
    {
        bool at_scheduler_safe_point = false;
        size_t connected_client_count = 0;
    };

    // These operations are owned by the executable or PIE host.  They must
    // release every host callback/resource that could enter module code before
    // the image is unloaded.  The session deliberately does not assume a
    // world, scheduler, renderer, network implementation, or game type.
    struct HostOperations
    {
        std::function<void()> quiesce;
        std::function<void()> release_module_resources;
        std::function<void(const std::string& source_id)> remove_module_registrations;
        // Cancels event, job, UI, and similar host callbacks registered by
        // this module. It runs after quiesce and before module shutdown while
        // callback destructors still have a loaded image. Appended to retain
        // aggregate initialization at existing executable call sites.
        std::function<void(const std::string& source_id)> release_module_callbacks;
        // Required for server-role reloads. An absent callback is an
        // unprovable server state and therefore refuses the reload.
        std::function<ServerReloadState()> query_server_reload_state;
        // After shutdown and resource release, prove every module job/event/UI callback is drained.
        // Absence or false refuses retirement while the image stays loaded.
        std::function<bool(const garden::ecs::ComponentOwnerId&)> module_callbacks_drained;
    };

    explicit GameModuleHostSession(HostOperations operations = {});
    ~GameModuleHostSession();

    GameModuleHostSession(const GameModuleHostSession&) = delete;
    GameModuleHostSession& operator=(const GameModuleHostSession&) = delete;

    // Loads a validated image and registers its reflected components.  The
    // caller then reconstructs/deserializes its world before initialising a
    // role.  Registration source ids use the loaded module's game name, which
    // matches GameModuleLoader's established factory cleanup convention.
    bool loadAndRegister(const std::string& module_path, ReflectionRegistry& reflection);

    // Client and server roles may coexist in a listen-server session.  A
    // failed init immediately follows the bounded teardown path; its role's
    // shutdown export is never called because initialization did not finish.
    // A failed module init must synchronously roll back every thread, callback
    // and resource it created before returning false.
    bool initialiseClient(EngineServices* services);
    bool initialiseServer(EngineServices* services);

    bool notifyClientLevelLoaded();
    bool notifyServerLevelLoaded();

    // Editor sessions pass true after all initialized roles have completed
    // their level callback.  Standalone Game/Server pass false and therefore
    // never receive the editor-only play notifications.
    bool beginPlaying(bool editor_session);

    // Validates B in a distinct owned generation while A remains dispatchable.
    // On success it shuts A down, promotes B, and recreates the recorded role
    // lifecycle.  B never receives a callback before A has fully stopped.
    // A failed validation leaves A Playing and callable.  A failed B
    // initialization after promotion leaves this host Idle by design.
    bool reloadCandidate(const std::string& module_path, ReflectionRegistry& reflection,
                         EngineServices* services);

    // The dispatch helpers become no-ops outside Playing so a host can remove
    // its scheduler callbacks at the same safe point as stop().
    void updateClient(float delta_time);
    void updateServerPrePhysics(float fixed_delta);
    void updateServer(float fixed_delta);

    // Performs the ordered quiesce -> optional play-stop -> role shutdown ->
    // host cleanup -> registration removal -> image unload sequence.  Safe to
    // call more than once.
    [[nodiscard]] bool stop();

    Phase phase() const { return m_phase; }
    bool isPlaying() const { return m_phase == Phase::Playing; }
    bool hasClientRole() const { return m_client_initialised; }
    bool hasServerRole() const { return m_server_initialised; }
    const std::string& sourceId() const { return m_source_id; }
    const std::string& lastFailure() const { return m_last_failure; }
    GameModuleLoader& loader() { return m_loader; }
    const GameModuleLoader& loader() const { return m_loader; }

private:
    bool canInitialiseRole() const;
    bool rolesHaveCompletedLevelLoad() const;
    bool activatePromotedCandidate(ReflectionRegistry& reflection, EngineServices* services,
                                  bool client_role, bool server_role, bool editor_session);
    void refreshReadyPhase();
    void failAndStop(const char* failure);

    HostOperations m_operations;
    GameModuleLoader m_loader;
    ReflectionRegistry* m_reflection = nullptr;
    std::string m_source_id;
    std::string m_last_failure;
    Phase m_phase = Phase::Idle;
    bool m_client_initialised = false;
    bool m_server_initialised = false;
    bool m_client_level_loaded = false;
    bool m_server_level_loaded = false;
    bool m_editor_session = false;
    bool m_play_started = false;
    bool m_stopping = false;
    bool m_quiesce_completed = false;
    bool m_callbacks_exposed = false;
    bool m_registrations_removed = false;
};
