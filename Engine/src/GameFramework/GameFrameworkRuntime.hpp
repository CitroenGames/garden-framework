#pragma once

#include <memory>
#include <string>

class world;

namespace GameFramework
{
class GameModeBase;
class GameStateBase;

// Owns the polymorphic framework objects and their world-wide lifecycle.
class GameFrameworkRuntime
{
public:
    GameFrameworkRuntime();
    ~GameFrameworkRuntime();

    // Ownership is installed before binding. A throwing bind/sync is logged;
    // the installed mode remains bound but its synchronization may be partial.
    void setAuthorityGameMode(world& game_world, std::unique_ptr<GameModeBase> mode);
    GameModeBase* getAuthorityGameMode() { return m_mode.get(); }
    const GameModeBase* getAuthorityGameMode() const { return m_mode.get(); }
    void setGameState(std::unique_ptr<GameStateBase> state);
    GameStateBase* getGameState() { return m_state.get(); }
    const GameStateBase* getGameState() const { return m_state.get(); }

    void clear();
    bool initialize(world& game_world, const std::string& map_name = "", const std::string& options = "");
    void start(world& game_world);
    void tick(world& game_world, float delta_time);
    // Clear lifecycle flags before invoking shutdown. Callback exceptions are
    // logged and suppressed; callback side effects are not reversible.
    void shutdown();
    bool isInitialized() const { return m_initialized; }
    bool hasStarted() const { return m_started; }

private:
    std::unique_ptr<GameModeBase> m_mode;
    std::unique_ptr<GameStateBase> m_state;
    bool m_initialized = false;
    bool m_started = false;
};
}
