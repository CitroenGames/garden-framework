#pragma once

#include "imgui.h"
#include "Graphics/RenderAPI.hpp"
#include <SDL.h>

// Forward declarations
union SDL_Event;

class ImGuiManager
{
public:
    static ImGuiManager& get();

    // Initialization - call after render API is initialized
    bool initialize(SDL_Window* window, IRenderAPI* renderAPI, RenderAPIType apiType);
    void shutdown();

    // Per-frame calls
    void newFrame();
    void render();

    // Event handling - returns true if ImGui consumed the event
    bool processEvent(const SDL_Event* event);

    // State queries
    bool isInitialized() const { return m_initialized; }
    bool wantCaptureMouse() const;
    bool wantCaptureKeyboard() const;

    // UI mode - when true, shows settings panel
    void setShowSettings(bool show) { m_showSettings = show; }
    bool getShowSettings() const { return m_showSettings; }


private:
    ImGuiManager() = default;
    ~ImGuiManager() = default;
    ImGuiManager(const ImGuiManager&) = delete;
    ImGuiManager& operator=(const ImGuiManager&) = delete;

    bool initOpenGL(SDL_Window* window, void* glContext);
    bool initVulkan(SDL_Window* window, IRenderAPI* vulkanAPI);
#ifdef _WIN32
    bool initD3D11(SDL_Window* window, IRenderAPI* d3d11API);
#endif

    bool m_initialized = false;
    RenderAPIType m_apiType = RenderAPIType::OpenGL;
    SDL_Window* m_window = nullptr;
    IRenderAPI* m_renderAPI = nullptr;
    bool m_showSettings = false;
};
