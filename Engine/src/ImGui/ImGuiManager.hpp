#pragma once

#include "imgui.h"
#include "Graphics/RenderAPI.hpp"
#include <SDL.h>
#include <vector>
#include <string>

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

    // Console
    void setShowConsole(bool show) { m_showConsole = show; }
    bool getShowConsole() const { return m_showConsole; }
    void toggleConsole() { m_showConsole = !m_showConsole; }

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

    void renderConsole();
    static int consoleInputCallback(ImGuiInputTextCallbackData* data);

    bool m_initialized = false;
    RenderAPIType m_apiType = RenderAPIType::OpenGL;
    SDL_Window* m_window = nullptr;
    IRenderAPI* m_renderAPI = nullptr;
    bool m_showSettings = false;

    // Console state
    bool m_showConsole = false;
    char m_consoleInput[512] = {0};
    int m_historyIndex = -1;
    bool m_scrollToBottom = false;
    int m_logLevelFilter = 0;  // 0 = all, 1 = warn+, 2 = error only

    // Autocomplete state
    std::vector<std::string> m_autocompleteItems;
    int m_autocompleteSelectedIndex = -1;
    bool m_showAutocomplete = false;
};
