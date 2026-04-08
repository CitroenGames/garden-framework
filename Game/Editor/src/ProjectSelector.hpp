#pragma once

#include <string>

// Lightweight project selector stage.
// Uses only SDL2 + SDL_Renderer + ImGui (no Vulkan/D3D11/etc).
// Returns the selected .garden project path, or empty string if user quit.
class ProjectSelector
{
public:
    // Run the project selector window. Blocks until a project is selected or window is closed.
    // Returns the selected/created .garden project file path, or empty string on quit.
    std::string run();
};
