#pragma once

struct EditorServices;

// Base class for plugin-contributed editor panels. The existing first-party
// panels (ConsolePanel, InspectorPanel, etc.) are NOT required to derive from
// this — they are drawn via hardcoded calls in EditorApp. This interface is
// the contract the dynamic panel draw loop uses for plugin panels.
class IEditorPanel
{
public:
    virtual ~IEditorPanel() = default;

    // Stable unique identifier. Used as:
    //   - ImGui window ID
    //   - Key for persisted visibility in editorconfig
    // Must not change across plugin reloads.
    virtual const char* getId() const = 0;

    // Human-readable name shown in the View menu.
    virtual const char* getDisplayName() const = 0;

    // "Left" | "Right" | "Center" | "Bottom" — hint for first-run docking.
    // The editor uses ImGui's DockBuilder to place the panel on initial
    // launch; subsequent launches honor the user's saved layout.
    virtual const char* getDefaultDockSlot() const { return "Right"; }

    // Whether the panel should be visible the first time a user installs
    // this plugin. After that, visibility persists in editorconfig.
    virtual bool isVisibleByDefault() const { return false; }

    // Called once after construction, before the first draw().
    virtual void onAttach(EditorServices* /*services*/) {}

    // Called before the panel is destroyed (on plugin unload / editor quit).
    virtual void onDetach() {}

    // Per-frame draw. `p_open` points to the panel's visibility bool and
    // should be forwarded to ImGui::Begin so users can close via the window
    // [x] button. Must be null-safe — the editor passes nullptr for
    // always-visible panels (rare).
    virtual void draw(bool* p_open) = 0;
};
