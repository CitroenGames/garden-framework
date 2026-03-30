#pragma once

#include <cstdint>

class IGameState
{
public:
    virtual ~IGameState() = default;

    // Called when this state becomes the active (top) state
    virtual void onEnter() = 0;

    // Called when this state is removed from the stack
    virtual void onExit() = 0;

    // Called when another state is pushed on top of this one
    virtual void onPause() {}

    // Called when the state above this one is popped
    virtual void onResume() {}

    // Per-frame update
    virtual void update(float dt) = 0;

    // Per-frame render (for overlay states)
    virtual void render() {}

    // If true, the state below this one still renders (e.g., pause menu overlay)
    virtual bool isTransparent() const { return false; }

    // If false, input is blocked from reaching states below
    virtual bool allowsInput() const { return true; }

    // Human-readable name for debugging
    virtual const char* getName() const = 0;
};
