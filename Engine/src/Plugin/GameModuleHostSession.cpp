#include "Plugin/GameModuleHostSession.hpp"

#include "Reflection/ReflectionRegistry.hpp"
#include "GameFramework/GameModeRegistry.hpp"

#include <utility>

GameModuleHostSession::GameModuleHostSession(HostOperations operations)
    : m_operations(std::move(operations))
{
}

GameModuleHostSession::~GameModuleHostSession()
{
    stop();
}

bool GameModuleHostSession::loadAndRegister(const std::string& module_path, ReflectionRegistry& reflection)
{
    if (m_phase != Phase::Idle || module_path.empty())
        return false;

    m_last_failure.clear();
    if (!m_loader.load(module_path))
    {
        m_last_failure = "module load or ABI validation failed";
        return false;
    }

    m_reflection = &reflection;
    m_source_id = m_loader.getGameName();
    if (m_source_id.empty() || m_source_id == "Unknown")
    {
        failAndStop("module did not provide a usable registration source id");
        return false;
    }

    m_loader.registerComponents(m_reflection);
    m_phase = Phase::Registered;
    return true;
}

bool GameModuleHostSession::canInitialiseRole() const
{
    return m_phase == Phase::Registered || m_phase == Phase::ClientInitialised ||
           m_phase == Phase::ServerInitialised || m_phase == Phase::Ready ||
           m_phase == Phase::InLevel;
}

bool GameModuleHostSession::initialiseClient(EngineServices* services)
{
    if (!canInitialiseRole() || m_client_initialised)
        return false;

    if (!m_loader.init(services))
    {
        failAndStop("client module initialization failed");
        return false;
    }

    m_client_initialised = true;
    refreshReadyPhase();
    return true;
}

bool GameModuleHostSession::initialiseServer(EngineServices* services)
{
    if (!canInitialiseRole() || m_server_initialised || !m_loader.hasServerSupport())
        return false;

    if (!m_loader.serverInit(services))
    {
        failAndStop("server module initialization failed");
        return false;
    }

    m_server_initialised = true;
    refreshReadyPhase();
    return true;
}

void GameModuleHostSession::refreshReadyPhase()
{
    if (m_client_initialised && m_server_initialised)
        m_phase = Phase::Ready;
    else if (m_client_initialised)
        m_phase = Phase::ClientInitialised;
    else if (m_server_initialised)
        m_phase = Phase::ServerInitialised;
}

bool GameModuleHostSession::notifyClientLevelLoaded()
{
    if (!m_client_initialised || m_client_level_loaded ||
        (m_phase != Phase::ClientInitialised && m_phase != Phase::Ready &&
         m_phase != Phase::ServerInitialised && m_phase != Phase::InLevel))
        return false;

    m_loader.onLevelLoaded();
    m_client_level_loaded = true;
    m_phase = Phase::InLevel;
    return true;
}

bool GameModuleHostSession::notifyServerLevelLoaded()
{
    if (!m_server_initialised || m_server_level_loaded ||
        (m_phase != Phase::ServerInitialised && m_phase != Phase::Ready &&
         m_phase != Phase::ClientInitialised && m_phase != Phase::InLevel))
        return false;

    m_loader.serverOnLevelLoaded();
    m_server_level_loaded = true;
    m_phase = Phase::InLevel;
    return true;
}

bool GameModuleHostSession::rolesHaveCompletedLevelLoad() const
{
    return (m_client_initialised || m_server_initialised) &&
           (!m_client_initialised || m_client_level_loaded) &&
           (!m_server_initialised || m_server_level_loaded);
}

bool GameModuleHostSession::beginPlaying(bool editor_session)
{
    if (m_phase != Phase::InLevel || !rolesHaveCompletedLevelLoad())
        return false;

    m_editor_session = editor_session;
    if (m_editor_session)
    {
        m_loader.onPlayStart();
        m_play_started = true;
    }
    m_phase = Phase::Playing;
    return true;
}

bool GameModuleHostSession::reloadCandidate(const std::string& module_path, ReflectionRegistry& reflection,
                                            EngineServices* services)
{
    if (m_phase != Phase::Playing || module_path.empty())
        return false;

    // Candidate owns a separate generation root and is fully ABI/export
    // validated before the active image is quiesced. No candidate callback is
    // legal before this point.
    GameModuleLoader candidate;
    if (!candidate.loadReloadCandidate(module_path))
    {
        m_last_failure = "module reload candidate validation failed";
        return false;
    }

    const bool client_role = m_client_initialised;
    const bool server_role = m_server_initialised;
    const bool editor_session = m_editor_session;
    stop();

    // stop() has released A's registrations and library. Moving transfers
    // B's handle and ownership lease without exposing an empty active loader
    // to a host tick.
    m_loader = std::move(candidate);
    return activatePromotedCandidate(reflection, services, client_role, server_role, editor_session);
}

bool GameModuleHostSession::activatePromotedCandidate(ReflectionRegistry& reflection, EngineServices* services,
                                                       bool client_role, bool server_role, bool editor_session)
{
    if (!m_loader.isLoaded())
        return false;

    m_last_failure.clear();
    m_reflection = &reflection;
    m_source_id = m_loader.getGameName();
    if (m_source_id.empty() || m_source_id == "Unknown")
    {
        failAndStop("promoted module did not provide a usable registration source id");
        return false;
    }

    m_loader.registerComponents(m_reflection);
    m_phase = Phase::Registered;

    // Keep server-before-client ordering for listen-server sessions, matching
    // the existing PIE lifecycle. Each role callback is paired during stop.
    if ((server_role && !initialiseServer(services)) ||
        (client_role && !initialiseClient(services)))
        return false;
    if ((server_role && !notifyServerLevelLoaded()) ||
        (client_role && !notifyClientLevelLoaded()) ||
        !beginPlaying(editor_session))
    {
        failAndStop("promoted module lifecycle reconstruction failed");
        return false;
    }
    return true;
}

void GameModuleHostSession::updateClient(float delta_time)
{
    if (m_phase == Phase::Playing && m_client_initialised)
        m_loader.update(delta_time);
}

void GameModuleHostSession::updateServerPrePhysics(float fixed_delta)
{
    if (m_phase == Phase::Playing && m_server_initialised)
        m_loader.serverPrePhysics(fixed_delta);
}

void GameModuleHostSession::updateServer(float fixed_delta)
{
    if (m_phase == Phase::Playing && m_server_initialised)
        m_loader.serverUpdate(fixed_delta);
}

void GameModuleHostSession::failAndStop(const char* failure)
{
    m_last_failure = failure;
    stop();
}

void GameModuleHostSession::stop()
{
    if (m_stopping || m_phase == Phase::Idle)
        return;

    m_stopping = true;
    m_phase = Phase::Quiescing;
    if (m_operations.quiesce)
        m_operations.quiesce();

    // Prevent every host callback or factory from retaining module code
    // across shutdown. These removals intentionally occur while the image is
    // still loaded, before any role shutdown callback or resource destructor.
    if (!m_source_id.empty())
    {
        if (m_operations.release_module_callbacks)
            m_operations.release_module_callbacks(m_source_id);
        GameFramework::GameModeRegistry::get().unregisterBySource(m_source_id);
        if (m_reflection)
            m_reflection->unregisterBySource(m_source_id.c_str());
        if (m_operations.remove_module_registrations)
            m_operations.remove_module_registrations(m_source_id);
    }

    // Callback code must remain loaded until every completed role has shut
    // down. A failed init deliberately leaves its role flag false.
    if (m_play_started)
        m_loader.onPlayStop();
    if (m_client_initialised)
        m_loader.shutdown();
    if (m_server_initialised)
        m_loader.serverShutdown();

    m_phase = Phase::Teardown;
    if (m_operations.release_module_resources)
        m_operations.release_module_resources();

    m_loader.unload();
    m_reflection = nullptr;
    m_source_id.clear();
    m_client_initialised = false;
    m_server_initialised = false;
    m_client_level_loaded = false;
    m_server_level_loaded = false;
    m_editor_session = false;
    m_play_started = false;
    m_phase = Phase::Idle;
    m_stopping = false;
}
