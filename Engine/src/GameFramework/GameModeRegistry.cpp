#include "GameFramework/GameModeRegistry.hpp"

#include "GameFramework/GameMode.hpp"
#include "GameFramework/GameModeBase.hpp"
#include "GameFramework/GameState.hpp"
#include "GameFramework/GameStateBase.hpp"

#include <algorithm>
#include <utility>

namespace GameFramework
{
GameModeRegistry& GameModeRegistry::get()
{
    static GameModeRegistry registry;
    return registry;
}

GameModeRegistry::GameModeRegistry()
{
    registerGameMode("GameModeBase", []() { return std::make_unique<GameModeBase>(); });
    registerGameMode("GameMode", []() { return std::make_unique<GameMode>(); });
    registerGameState("GameStateBase", []() { return std::make_unique<GameStateBase>(); });
    registerGameState("GameState", []() { return std::make_unique<GameState>(); });
}

bool GameModeRegistry::registerGameMode(std::string name, GameModeFactory factory)
{
    if (name.empty() || !factory)
        return false;

    m_game_mode_factories[std::move(name)] = std::move(factory);
    return true;
}

bool GameModeRegistry::registerGameState(std::string name, GameStateFactory factory)
{
    if (name.empty() || !factory)
        return false;

    m_game_state_factories[std::move(name)] = std::move(factory);
    return true;
}

std::unique_ptr<GameModeBase> GameModeRegistry::createGameMode(const std::string& name) const
{
    auto it = m_game_mode_factories.find(name);
    if (it != m_game_mode_factories.end())
        return it->second();

    auto fallback = m_game_mode_factories.find("GameMode");
    return fallback != m_game_mode_factories.end() ? fallback->second() : nullptr;
}

std::unique_ptr<GameStateBase> GameModeRegistry::createGameState(const std::string& name) const
{
    auto it = m_game_state_factories.find(name);
    if (it != m_game_state_factories.end())
        return it->second();

    auto fallback = m_game_state_factories.find("GameState");
    return fallback != m_game_state_factories.end() ? fallback->second() : nullptr;
}

bool GameModeRegistry::hasGameMode(const std::string& name) const
{
    return m_game_mode_factories.find(name) != m_game_mode_factories.end();
}

bool GameModeRegistry::hasGameState(const std::string& name) const
{
    return m_game_state_factories.find(name) != m_game_state_factories.end();
}

std::vector<std::string> GameModeRegistry::getGameModeNames() const
{
    std::vector<std::string> names;
    names.reserve(m_game_mode_factories.size());
    for (const auto& [name, factory] : m_game_mode_factories)
    {
        (void)factory;
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<std::string> GameModeRegistry::getGameStateNames() const
{
    std::vector<std::string> names;
    names.reserve(m_game_state_factories.size());
    for (const auto& [name, factory] : m_game_state_factories)
    {
        (void)factory;
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}
}
