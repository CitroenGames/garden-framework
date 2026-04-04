#pragma once

#include "EngineGraphicsExport.h"
#include "Graphics/RenderAPI.hpp"
#include <SDL.h>

// Forward declarations
namespace Rml { class Context; class RenderInterface; }
class SystemInterface_SDL;

class ENGINE_GRAPHICS_API RmlUiManager
{
public:
    static RmlUiManager& get();

    // Initialization - call after render API is initialized
    bool initialize(SDL_Window* window, IRenderAPI* renderAPI, RenderAPIType apiType);
    void shutdown();

    // Per-frame calls
    void beginFrame();
    void render();

    // Event handling - returns true if RmlUi consumed the event
    bool processEvent(SDL_Event& event);

    // State queries
    bool isInitialized() const { return m_initialized; }

    // Context access
    Rml::Context* getContext() const { return m_context; }

    // Document management
    void* loadDocument(const char* path);
    void toggleDebugger();

private:
    RmlUiManager() = default;
    ~RmlUiManager() = default;
    RmlUiManager(const RmlUiManager&) = delete;
    RmlUiManager& operator=(const RmlUiManager&) = delete;

    bool initD3D11(SDL_Window* window, IRenderAPI* api);
    bool initVulkan(SDL_Window* window, IRenderAPI* api);
    bool initMetal(SDL_Window* window, IRenderAPI* api);

    bool m_initialized = false;
    RenderAPIType m_apiType = DefaultRenderAPI;
    SDL_Window* m_window = nullptr;
    IRenderAPI* m_renderAPI = nullptr;

    Rml::Context* m_context = nullptr;
    Rml::RenderInterface* m_renderInterface = nullptr;
    SystemInterface_SDL* m_systemInterface = nullptr;
    bool m_debuggerVisible = false;
};
