#include "GameStateManager.hpp"
#include "Events/EventBus.hpp"
#include "Events/EngineEvents.hpp"
#include "Utils/Log.hpp"

void GameStateManager::pushState(std::unique_ptr<IGameState> state)
{
    if (!state) return;

    // Pause current top state
    if (!states.empty())
    {
        states.back()->onPause();
    }

    const char* name = state->getName();
    LOG_ENGINE_INFO("GameState: pushing '{}'", name);

    states.push_back(std::move(state));
    states.back()->onEnter();

    EventBus::get().queue(StatePushedEvent{name});
}

void GameStateManager::popState()
{
    if (states.empty()) return;

    std::string old_name = states.back()->getName();
    LOG_ENGINE_INFO("GameState: popping '{}'", old_name);

    states.back()->onExit();
    states.pop_back();

    // Resume the state that's now on top
    if (!states.empty())
    {
        states.back()->onResume();

        EventBus::get().queue(StateChangedEvent{old_name, states.back()->getName()});
    }

    EventBus::get().queue(StatePoppedEvent{old_name});
}

void GameStateManager::switchState(std::unique_ptr<IGameState> state)
{
    if (!state) return;

    std::string old_name;
    if (!states.empty())
    {
        old_name = states.back()->getName();
        states.back()->onExit();
        states.pop_back();
    }

    const char* new_name = state->getName();
    LOG_ENGINE_INFO("GameState: switching '{}' -> '{}'", old_name, new_name);

    states.push_back(std::move(state));
    states.back()->onEnter();

    EventBus::get().queue(StateChangedEvent{old_name, new_name});
}

void GameStateManager::update(float dt)
{
    if (states.empty()) return;

    // Only update the top state
    states.back()->update(dt);
}

void GameStateManager::render()
{
    if (states.empty()) return;

    // Find the lowest state that needs to render
    // Walk from top down to find the first non-transparent state
    int render_from = static_cast<int>(states.size()) - 1;
    for (int i = static_cast<int>(states.size()) - 1; i > 0; i--)
    {
        if (!states[i]->isTransparent())
        {
            render_from = i;
            break;
        }
        render_from = i - 1;
    }

    // Render from bottom-most visible state upward
    for (int i = render_from; i < static_cast<int>(states.size()); i++)
    {
        states[i]->render();
    }
}

IGameState* GameStateManager::getCurrentState() const
{
    if (states.empty()) return nullptr;
    return states.back().get();
}

bool GameStateManager::isInState(const char* name) const
{
    for (const auto& state : states)
    {
        if (std::string(state->getName()) == name)
            return true;
    }
    return false;
}

void GameStateManager::clear()
{
    while (!states.empty())
    {
        states.back()->onExit();
        states.pop_back();
    }
}
