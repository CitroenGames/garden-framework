#pragma once

#include "imgui.h"
#include "EditorState.hpp"

class ToolbarPanel;

class ViewportPanel
{
public:
    // Current viewport content size (used by EditorApp to resize render target)
    int width = 800;
    int height = 600;

    // Set by EditorApp during initialization
    ToolbarPanel* toolbar = nullptr;
    bool* show_toolbar = nullptr;

    void draw(ImTextureID scene_texture, EditorState& state);
};
