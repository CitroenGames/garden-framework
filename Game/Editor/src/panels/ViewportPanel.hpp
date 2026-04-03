#pragma once

#include "imgui.h"
#include "EditorState.hpp"

class ViewportPanel
{
public:
    // Current viewport content size (used by EditorApp to resize render target)
    int width = 800;
    int height = 600;

    void draw(ImTextureID scene_texture, PlayMode play_mode = PlayMode::Editing);
};
