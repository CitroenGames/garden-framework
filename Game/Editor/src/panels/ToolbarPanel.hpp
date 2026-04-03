#pragma once

#include <functional>

struct EditorState;

struct ToolbarCallbacks
{
    std::function<void()> on_play;     // Play pressed while Editing
    std::function<void()> on_pause;    // Pause pressed while Playing
    std::function<void()> on_resume;   // Play/Resume pressed while Paused
    std::function<void()> on_stop;     // Stop pressed (any active state)
    std::function<void()> on_eject;    // Eject pressed while Playing
    std::function<void()> on_return;   // Return pressed while Ejected
};

class ToolbarPanel
{
public:
    ToolbarCallbacks callbacks;
    void draw(EditorState& state);
    void drawContent(EditorState& state);
};
