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
#include "RmlRenderer_D3D12.h"
#include "Graphics/D3D12RenderAPI.hpp"
#endif
#ifdef __APPLE__
#include "RmlRenderer_Metal.h"
#include "Graphics/MetalRenderAPI.hpp"
#endif
#include "Graphics/VulkanRenderAPI.hpp"

#include <algorithm>
#include <string>
#include <unordered_map>

namespace
{
    struct RmlUiDataModel
    {
        std::string name;
        Rml::DataModelConstructor constructor;
        Rml::DataModelHandle handle;
        std::unordered_map<std::string, int> ints;
        std::unordered_map<std::string, bool> bools;
        std::unordered_map<std::string, Rml::String> strings;
    };

    RmlUiDataModel* asDataModel(void* model)
    {
        return static_cast<RmlUiDataModel*>(model);
    }

    bool isValidName(const char* name)
    {
        return name && name[0] != '\0';
    }

    void deleteDataModel(Rml::Context* context, RmlUiDataModel* model)
    {
        if (!model)
            return;

        if (context && !model->name.empty())
            context->RemoveDataModel(model->name);

        delete model;
    }
}

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
    else if (apiType == RenderAPIType::D3D12)
    {
        success = initD3D12(window, renderAPI);
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

    // Create main/game context
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

    // Editor chrome gets a separate context so documents rendered into the
    // editor swapchain do not duplicate game/PIE documents rendered into scene
    // viewports.
    m_editorContext = Rml::CreateContext("editor", Rml::Vector2i(w, h));
    if (!m_editorContext)
    {
        LOG_ENGINE_ERROR("[RmlUi] Failed to create editor context");
        Rml::RemoveContext(m_context->GetName());
        m_context = nullptr;
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

bool RmlUiManager::isInitialized() const
{
    return m_initialized;
}

Rml::Context* RmlUiManager::getContext() const
{
    return m_context;
}

Rml::Context* RmlUiManager::getEditorContext() const
{
    return m_editorContext;
}

#ifdef _WIN32
bool RmlUiManager::initD3D12(SDL_Window* window, IRenderAPI* api)
{
    (void)window;
    auto* d3dAPI = dynamic_cast<D3D12RenderAPI*>(api);
    if (!d3dAPI)
        return false;

    auto* renderer = new RmlRenderer_D3D12();
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

    for (void* document : m_documents)
    {
        if (document)
            static_cast<Rml::ElementDocument*>(document)->Close();
    }
    m_documents.clear();

    for (void* document : m_editorDocuments)
    {
        if (document)
            static_cast<Rml::ElementDocument*>(document)->Close();
    }
    m_editorDocuments.clear();

    for (void* model : m_dataModels)
        deleteDataModel(m_context, asDataModel(model));
    m_dataModels.clear();

    Rml::Debugger::Shutdown();

    if (m_context)
    {
        Rml::RemoveContext(m_context->GetName());
        m_context = nullptr;
    }

    if (m_editorContext)
    {
        Rml::RemoveContext(m_editorContext->GetName());
        m_editorContext = nullptr;
    }

    Rml::Shutdown();

    if (m_renderInterface)
    {
        // Shutdown specific renderer
        if (m_apiType == RenderAPIType::Vulkan)
            static_cast<RmlRenderer_VK*>(m_renderInterface)->Shutdown();
#ifdef _WIN32
        else if (m_apiType == RenderAPIType::D3D12)
            static_cast<RmlRenderer_D3D12*>(m_renderInterface)->Shutdown();
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

    int w, h;
    SDL_GetWindowSize(m_window, &w, &h);
    beginFrame(w, h);
}

void RmlUiManager::beginFrame(int width, int height)
{
    if (!m_initialized || !m_context || width <= 0 || height <= 0)
        return;

    m_context->SetDimensions(Rml::Vector2i(width, height));

    // Update renderer viewport
    if (m_apiType == RenderAPIType::Vulkan)
    {
        auto* vkRenderer = static_cast<RmlRenderer_VK*>(m_renderInterface);
        vkRenderer->SetViewport(width, height);
        vkRenderer->BeginFrame();
    }
#ifdef _WIN32
    else if (m_apiType == RenderAPIType::D3D12)
    {
        auto* d3dRenderer = static_cast<RmlRenderer_D3D12*>(m_renderInterface);
        d3dRenderer->SetViewport(width, height);
        d3dRenderer->BeginFrame();
    }
#endif
#ifdef __APPLE__
    else if (m_apiType == RenderAPIType::Metal)
    {
        auto* metalRenderer = static_cast<RmlRenderer_Metal*>(m_renderInterface);
        metalRenderer->SetViewport(width, height);
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

void RmlUiManager::beginEditorFrame(int width, int height)
{
    if (!m_initialized || !m_editorContext || width <= 0 || height <= 0)
        return;

    m_editorContext->SetDimensions(Rml::Vector2i(width, height));

    if (m_apiType == RenderAPIType::Vulkan)
    {
        auto* vkRenderer = static_cast<RmlRenderer_VK*>(m_renderInterface);
        vkRenderer->SetViewport(width, height);
        vkRenderer->BeginFrame();
    }
#ifdef _WIN32
    else if (m_apiType == RenderAPIType::D3D12)
    {
        auto* d3dRenderer = static_cast<RmlRenderer_D3D12*>(m_renderInterface);
        d3dRenderer->SetViewport(width, height);
        d3dRenderer->BeginFrame();
    }
#endif
#ifdef __APPLE__
    else if (m_apiType == RenderAPIType::Metal)
    {
        auto* metalRenderer = static_cast<RmlRenderer_Metal*>(m_renderInterface);
        metalRenderer->SetViewport(width, height);
        metalRenderer->BeginFrame();
    }
#endif
}

void RmlUiManager::renderEditor()
{
    if (!m_initialized || !m_editorContext)
        return;

    m_editorContext->Update();
    m_editorContext->Render();
}

bool RmlUiManager::processEvent(SDL_Event& event)
{
    if (!m_initialized || !m_context)
        return false;

    return !RmlSDL::InputEventHandler(m_context, m_window, event);
}

bool RmlUiManager::processEditorEvent(SDL_Event& event)
{
    if (!m_initialized || !m_editorContext)
        return false;

    return !RmlSDL::InputEventHandler(m_editorContext, m_window, event);
}

void* RmlUiManager::loadDocument(const char* path)
{
    if (!m_initialized || !m_context)
        return nullptr;

    Rml::ElementDocument* doc = m_context->LoadDocument(path ? path : "");
    if (doc)
    {
        doc->Show();
        m_documents.push_back(doc);
    }
    return doc;
}

void* RmlUiManager::loadEditorDocument(const char* path)
{
    if (!m_initialized || !m_editorContext)
        return nullptr;

    Rml::ElementDocument* doc = m_editorContext->LoadDocument(path ? path : "");
    if (doc)
    {
        doc->Show();
        m_editorDocuments.push_back(doc);
    }
    return doc;
}

void RmlUiManager::closeDocument(void* document)
{
    if (!document)
        return;

    auto it = std::remove(m_documents.begin(), m_documents.end(), document);
    m_documents.erase(it, m_documents.end());

    if (m_context)
        static_cast<Rml::ElementDocument*>(document)->Close();
}

void RmlUiManager::closeEditorDocument(void* document)
{
    if (!document)
        return;

    auto it = std::remove(m_editorDocuments.begin(), m_editorDocuments.end(), document);
    m_editorDocuments.erase(it, m_editorDocuments.end());

    if (m_editorContext)
        static_cast<Rml::ElementDocument*>(document)->Close();
}

void RmlUiManager::toggleDebugger()
{
    if (!m_initialized)
        return;

    m_debuggerVisible = !m_debuggerVisible;
    Rml::Debugger::SetVisible(m_debuggerVisible);
}

void* RmlUiManager::createDataModel(const char* name)
{
    if (!m_initialized || !m_context || !isValidName(name))
        return nullptr;

    Rml::DataModelConstructor constructor = m_context->CreateDataModel(name);
    if (!constructor)
        return nullptr;

    auto* model = new RmlUiDataModel();
    model->name = name;
    model->constructor = constructor;
    model->handle = constructor.GetModelHandle();
    m_dataModels.push_back(model);
    return model;
}

void RmlUiManager::removeDataModel(void* model)
{
    if (!model)
        return;

    auto it = std::remove(m_dataModels.begin(), m_dataModels.end(), model);
    m_dataModels.erase(it, m_dataModels.end());

    deleteDataModel(m_context, asDataModel(model));
}

bool RmlUiManager::dataModelBindInt(void* model_handle, const char* name, int value)
{
    RmlUiDataModel* model = asDataModel(model_handle);
    if (!model || !isValidName(name))
        return false;

    auto [it, inserted] = model->ints.emplace(name, value);
    if (!inserted)
    {
        it->second = value;
        return true;
    }

    if (!model->constructor.Bind(it->first, &it->second))
    {
        model->ints.erase(it);
        return false;
    }
    return true;
}

bool RmlUiManager::dataModelBindBool(void* model_handle, const char* name, bool value)
{
    RmlUiDataModel* model = asDataModel(model_handle);
    if (!model || !isValidName(name))
        return false;

    auto [it, inserted] = model->bools.emplace(name, value);
    if (!inserted)
    {
        it->second = value;
        return true;
    }

    if (!model->constructor.Bind(it->first, &it->second))
    {
        model->bools.erase(it);
        return false;
    }
    return true;
}

bool RmlUiManager::dataModelBindString(void* model_handle, const char* name, const char* value)
{
    RmlUiDataModel* model = asDataModel(model_handle);
    if (!model || !isValidName(name))
        return false;

    auto [it, inserted] = model->strings.emplace(name, value ? value : "");
    if (!inserted)
    {
        it->second = value ? value : "";
        return true;
    }

    if (!model->constructor.Bind(it->first, &it->second))
    {
        model->strings.erase(it);
        return false;
    }
    return true;
}

bool RmlUiManager::dataModelSetInt(void* model_handle, const char* name, int value)
{
    RmlUiDataModel* model = asDataModel(model_handle);
    if (!model || !isValidName(name))
        return false;

    auto it = model->ints.find(name);
    if (it == model->ints.end())
        return false;

    it->second = value;
    return true;
}

bool RmlUiManager::dataModelSetBool(void* model_handle, const char* name, bool value)
{
    RmlUiDataModel* model = asDataModel(model_handle);
    if (!model || !isValidName(name))
        return false;

    auto it = model->bools.find(name);
    if (it == model->bools.end())
        return false;

    it->second = value;
    return true;
}

bool RmlUiManager::dataModelSetString(void* model_handle, const char* name, const char* value)
{
    RmlUiDataModel* model = asDataModel(model_handle);
    if (!model || !isValidName(name))
        return false;

    auto it = model->strings.find(name);
    if (it == model->strings.end())
        return false;

    it->second = value ? value : "";
    return true;
}

void RmlUiManager::dataModelDirtyAll(void* model_handle)
{
    RmlUiDataModel* model = asDataModel(model_handle);
    if (!model || !model->handle)
        return;

    model->handle.DirtyAllVariables();
}
