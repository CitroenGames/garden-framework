#pragma once

#include "IGameState.hpp"
#include <vector>
#include <memory>
#include <string>

class GameStateManager
{
public:
    static GameStateManager& get()
    {
        static GameStateManager instance;
        return instance;
    }

    // Push a new state onto the stack (becomes active)
    void pushState(std::unique_ptr<IGameState> state);

    // Pop the current top state
    void popState();

    // Replace the top state (pop + push)
    void switchState(std::unique_ptr<IGameState> state);

    // Update the active state(s)
    void update(float dt);

    // Render the state stack (bottom-up for transparent states)
    void render();

    // Query
    IGameState* getCurrentState() const;
    bool isInState(const char* name) const;
    bool isEmpty() const { return states.empty(); }
    size_t getStackDepth() const { return states.size(); }

    // Clear all states (for shutdown)
    void clear();

private:
    GameStateManager() = default;
    ~GameStateManager() = default;
    GameStateManager(const GameStateManager&) = delete;
    GameStateManager& operator=(const GameStateManager&) = delete;

    std::vector<std::unique_ptr<IGameState>> states;
};
