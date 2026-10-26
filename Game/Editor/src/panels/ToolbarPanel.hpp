#pragma once

#include <functional>
#include <string>

struct EditorState;

// ImGui play/transform toolbar. Play/Pause/Stop/Eject, the play-options
// menu (launch mode, spawn, multiplayer, VR) and the viewport transform
// tools all live here — there is no other editor chrome.
struct ToolbarCallbacks
{
    std::function<void()> on_play;     // Play pressed while Editing
    std::function<void()> on_pause;    // Pause pressed while Playing
    std::function<void()> on_resume;   // Play/Resume pressed while Paused
    std::function<void()> on_stop;     // Stop pressed (any active state)
    std::function<void()> on_eject;    // Eject pressed while Playing
    std::function<void()> on_return;   // Return pressed while Ejected
    std::function<void()> on_quick_launch_vr;
    std::function<void()> on_refresh_openxr;
    std::function<void()> on_save;
    std::function<void()> on_settings;
};

class ToolbarPanel
{
public:
    ToolbarCallbacks callbacks;
    bool has_game_module = false;      // true when project has a loadable game DLL
    bool external_pie_active = false;  // set by EditorApp each frame
    std::string xr_launch_label = "Meta Quest Link";
    std::string xr_runtime_label = "Runtime: not initialized";
    std::string xr_hmd_label = "HMD: unavailable";
    std::string xr_error;

    void draw(EditorState& state);
    void drawContent(EditorState& state);
    // Play/Pause/Resume/Stop/Eject/Return row for the fixed main toolbar
    // above the viewport (not part of the viewport strip).
    void drawPlayControls(EditorState& state);
    // Launch/spawn/multiplayer/VR menu items, shared by the toolbar
    // popup and the main menu bar's Play menu.
    void drawPlayMenuItems(EditorState& state);
};
