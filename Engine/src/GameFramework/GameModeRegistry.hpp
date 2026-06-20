#pragma once

#include "EngineExport.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace GameFramework
{
class GameModeBase;
class GameStateBase;

class ENGINE_API GameModeRegistry
{
public:
    using GameModeFactory = std::function<std::unique_ptr<GameModeBase>()>;
    using GameStateFactory = std::function<std::unique_ptr<GameStateBase>()>;

    static GameModeRegistry& get();

    bool registerGameMode(std::string name, GameModeFactory factory, std::string source_id = "engine");
    bool registerGameState(std::string name, GameStateFactory factory, std::string source_id = "engine");
    void unregisterBySource(const std::string& source_id);

    std::unique_ptr<GameModeBase> createGameMode(const std::string& name) const;
    std::unique_ptr<GameStateBase> createGameState(const std::string& name) const;

    bool hasGameMode(const std::string& name) const;
    bool hasGameState(const std::string& name) const;

    std::vector<std::string> getGameModeNames() const;
    std::vector<std::string> getGameStateNames() const;

private:
    GameModeRegistry();

    struct GameModeEntry
    {
        GameModeFactory factory;
        std::string source_id;
    };

    struct GameStateEntry
    {
        GameStateFactory factory;
        std::string source_id;
    };

    std::unordered_map<std::string, GameModeEntry> m_game_mode_factories;
    std::unordered_map<std::string, GameStateEntry> m_game_state_factories;
};
}
