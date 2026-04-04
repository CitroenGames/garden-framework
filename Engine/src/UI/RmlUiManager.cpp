#include "RmlUiManager.h"
#include "Utils/Log.hpp"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Traits.h>
#include <RmlUi/Debugger.h>

// Force Rml::FamilyBase::GetNewId into this DLL so game modules can use DataModelConstructor::Bind
static volatile auto s_force_rml_family = &Rml::Family<int>::Id;
#include "Utils/EnginePaths.hpp"
#include <RmlUi_Platform_SDL.h>

// Render interface headers (platform-guarded)
#include "RmlRenderer_VK.h"
#ifdef _WIN32
#include "RmlRenderer_D3D11.h"
#include "Graphics/D3D11RenderAPI.hpp"
#endif
#ifdef __APPLE__
#include "RmlRenderer_Metal.h"
#include "Graphics/MetalRenderAPI.hpp"
#endif
#include "Graphics/VulkanRenderAPI.hpp"

RmlUiManager& RmlUiManager::get()
{
    static RmlUiManager instance;
    return instance;
}

bool RmlUiManager::initialize(SDL_Window* window, IRenderAPI* renderAPI, RenderAPIType apiType)
{
    if (m_initialized)
        return true;

    m_window = window;
    m_renderAPI = renderAPI;
    m_apiType = apiType;

    // Create system interface (SDL backend)
    m_systemInterface = new SystemInterface_SDL();
    m_systemInterface->SetWindow(window);

    // Create render interface based on API type
    bool success = false;
    if (apiType == RenderAPIType::Vulkan)
    {
        success = initVulkan(window, renderAPI);
    }
#ifdef _WIN32
    else if (apiType == RenderAPIType::D3D11)
    {
        success = initD3D11(window, renderAPI);
    }
#endif
#ifdef __APPLE__
    else if (apiType == RenderAPIType::Metal)
    {
        success = initMetal(window, renderAPI);
    }
#endif

    if (!success)
    {
        delete m_systemInterface;
        m_systemInterface = nullptr;
        LOG_ENGINE_ERROR("[RmlUi] Failed to initialize render interface");
        return false;
    }

    // Initialize RmlUi core
    Rml::SetSystemInterface(m_systemInterface);
    Rml::SetRenderInterface(m_renderInterface);

    if (!Rml::Initialise())
    {
        LOG_ENGINE_ERROR("[RmlUi] Failed to initialise RmlUi core");
        delete m_renderInterface;
        m_renderInterface = nullptr;
        delete m_systemInterface;
        m_systemInterface = nullptr;
        return false;
    }

    // Get window dimensions
    int w, h;
    SDL_GetWindowSize(window, &w, &h);

    // Create main context
    m_context = Rml::CreateContext("main", Rml::Vector2i(w, h));
    if (!m_context)
    {
        LOG_ENGINE_ERROR("[RmlUi] Failed to create context");
        Rml::Shutdown();
        delete m_renderInterface;
        m_renderInterface = nullptr;
        delete m_systemInterface;
        m_systemInterface = nullptr;
        return false;
    }

    // Load default fonts
    std::string fontDir = EnginePaths::resolveEngineAsset("../assets/fonts/");
    Rml::LoadFontFace(fontDir + "LatoLatin-Regular.ttf", true);
    Rml::LoadFontFace(fontDir + "LatoLatin-Bold.ttf", true);

    // Initialize debugger
    Rml::Debugger::Initialise(m_context);

    m_initialized = true;
    LOG_ENGINE_INFO("[RmlUi] Initialized successfully with {} backend", renderAPI->getAPIName());
    return true;
}

#ifdef _WIN32
bool RmlUiManager::initD3D11(SDL_Window* window, IRenderAPI* api)
{
    (void)window;
    auto* d3dAPI = dynamic_cast<D3D11RenderAPI*>(api);
    if (!d3dAPI)
        return false;

    auto* renderer = new RmlRenderer_D3D11();
    if (!renderer->Init(d3dAPI))
    {
        delete renderer;
        return false;
    }

    int w, h;
    SDL_GetWindowSize(m_window, &w, &h);
    renderer->SetViewport(w, h);

    m_renderInterface = renderer;
    return true;
}
#endif

bool RmlUiManager::initVulkan(SDL_Window* window, IRenderAPI* api)
{
    (void)window;
    auto* vkAPI = dynamic_cast<VulkanRenderAPI*>(api);
    if (!vkAPI)
        return false;

    auto* renderer = new RmlRenderer_VK();
    if (!renderer->Init(vkAPI))
    {
        delete renderer;
        return false;
    }

    int w, h;
    SDL_GetWindowSize(m_window, &w, &h);
    renderer->SetViewport(w, h);

    m_renderInterface = renderer;
    return true;
}

#ifdef __APPLE__
bool RmlUiManager::initMetal(SDL_Window* window, IRenderAPI* api)
{
    (void)window;
    auto* metalAPI = dynamic_cast<MetalRenderAPI*>(api);
    if (!metalAPI)
        return false;

    auto* renderer = new RmlRenderer_Metal();
    if (!renderer->Init(metalAPI))
    {
        delete renderer;
        return false;
    }

    int w, h;
    SDL_GetWindowSize(m_window, &w, &h);
    renderer->SetViewport(w, h);

    m_renderInterface = renderer;
    return true;
}
#endif

void RmlUiManager::shutdown()
{
    if (!m_initialized)
        return;

    Rml::Debugger::Shutdown();

    if (m_context)
    {
        Rml::RemoveContext(m_context->GetName());
        m_context = nullptr;
    }

    Rml::Shutdown();

    if (m_renderInterface)
    {
        // Shutdown specific renderer
        if (m_apiType == RenderAPIType::Vulkan)
            static_cast<RmlRenderer_VK*>(m_renderInterface)->Shutdown();
#ifdef _WIN32
        else if (m_apiType == RenderAPIType::D3D11)
            static_cast<RmlRenderer_D3D11*>(m_renderInterface)->Shutdown();
#endif
#ifdef __APPLE__
        else if (m_apiType == RenderAPIType::Metal)
            static_cast<RmlRenderer_Metal*>(m_renderInterface)->Shutdown();
#endif
        delete m_renderInterface;
        m_renderInterface = nullptr;
    }

    delete m_systemInterface;
    m_systemInterface = nullptr;

    m_initialized = false;
    LOG_ENGINE_INFO("[RmlUi] Shutdown complete");
}

void RmlUiManager::beginFrame()
{
    if (!m_initialized || !m_context)
        return;

    // Update viewport dimensions
    int w, h;
    SDL_GetWindowSize(m_window, &w, &h);
    m_context->SetDimensions(Rml::Vector2i(w, h));

    // Update renderer viewport
    if (m_apiType == RenderAPIType::Vulkan)
    {
        auto* vkRenderer = static_cast<RmlRenderer_VK*>(m_renderInterface);
        vkRenderer->SetViewport(w, h);
        vkRenderer->BeginFrame();
    }
#ifdef _WIN32
    else if (m_apiType == RenderAPIType::D3D11)
    {
        static_cast<RmlRenderer_D3D11*>(m_renderInterface)->SetViewport(w, h);
    }
#endif
#ifdef __APPLE__
    else if (m_apiType == RenderAPIType::Metal)
    {
        auto* metalRenderer = static_cast<RmlRenderer_Metal*>(m_renderInterface);
        metalRenderer->SetViewport(w, h);
        metalRenderer->BeginFrame();
    }
#endif
}

void RmlUiManager::render()
{
    if (!m_initialized || !m_context)
        return;

    m_context->Update();
    m_context->Render();
}

bool RmlUiManager::processEvent(SDL_Event& event)
{
    if (!m_initialized || !m_context)
        return false;

    return !RmlSDL::InputEventHandler(m_context, m_window, event);
}

void* RmlUiManager::loadDocument(const char* path)
{
    if (!m_initialized || !m_context)
        return nullptr;

    Rml::ElementDocument* doc = m_context->LoadDocument(path);
    if (doc)
        doc->Show();
    return doc;
}

void RmlUiManager::toggleDebugger()
{
    if (!m_initialized)
        return;

    m_debuggerVisible = !m_debuggerVisible;
    Rml::Debugger::SetVisible(m_debuggerVisible);
}
