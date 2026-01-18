#include "ImGuiManager.hpp"
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_vulkan.h"
#ifdef _WIN32
#include "imgui_impl_dx11.h"
#include "Graphics/D3D11RenderAPI.hpp"
#endif
#include "Graphics/VulkanRenderAPI.hpp"
#include <SDL.h>

ImGuiManager& ImGuiManager::get()
{
    static ImGuiManager instance;
    return instance;
}

bool ImGuiManager::initialize(SDL_Window* window, IRenderAPI* renderAPI, RenderAPIType apiType)
{
    if (m_initialized) return true;

    m_window = window;
    m_renderAPI = renderAPI;
    m_apiType = apiType;

    // Create ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // Multi-viewport can be enabled later if needed:
    // io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

    // Setup style
    ImGui::StyleColorsDark();

    // Initialize platform and renderer backends
    bool success = false;
    if (apiType == RenderAPIType::OpenGL)
    {
        success = initOpenGL(window, SDL_GL_GetCurrentContext());
    }
    else if (apiType == RenderAPIType::Vulkan)
    {
        success = initVulkan(window, renderAPI);
    }
#ifdef _WIN32
    else if (apiType == RenderAPIType::D3D11)
    {
        success = initD3D11(window, renderAPI);
    }
#endif

    m_initialized = success;
    return success;
}

bool ImGuiManager::initOpenGL(SDL_Window* window, void* glContext)
{
    // Initialize SDL2 backend for OpenGL
    if (!ImGui_ImplSDL2_InitForOpenGL(window, glContext))
    {
        return false;
    }

    // Initialize OpenGL3 backend with GLSL version for OpenGL 4.6
    const char* glsl_version = "#version 460";
    if (!ImGui_ImplOpenGL3_Init(glsl_version))
    {
        ImGui_ImplSDL2_Shutdown();
        return false;
    }

    return true;
}

bool ImGuiManager::initVulkan(SDL_Window* window, IRenderAPI* vulkanAPI)
{
    // Initialize SDL2 backend for Vulkan
    if (!ImGui_ImplSDL2_InitForVulkan(window))
    {
        return false;
    }

    // Cast to VulkanRenderAPI to access Vulkan handles
    VulkanRenderAPI* vkAPI = dynamic_cast<VulkanRenderAPI*>(vulkanAPI);
    if (!vkAPI)
    {
        ImGui_ImplSDL2_Shutdown();
        return false;
    }

    // Get the render pass - prefer FXAA pass for crisp text, fall back to main pass
    VkRenderPass renderPass = vkAPI->getFxaaRenderPass();
    if (renderPass == VK_NULL_HANDLE)
    {
        renderPass = vkAPI->getRenderPass();
    }
    if (renderPass == VK_NULL_HANDLE)
    {
        ImGui_ImplSDL2_Shutdown();
        return false;
    }

    // Fill ImGui_ImplVulkan_InitInfo
    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.ApiVersion = VK_API_VERSION_1_2;
    init_info.Instance = vkAPI->getInstance();
    init_info.PhysicalDevice = vkAPI->getPhysicalDevice();
    init_info.Device = vkAPI->getDevice();
    init_info.QueueFamily = vkAPI->getGraphicsQueueFamily();
    init_info.Queue = vkAPI->getGraphicsQueue();
    init_info.RenderPass = renderPass;
    init_info.MinImageCount = 2;
    init_info.ImageCount = vkAPI->getSwapchainImageCount();
    init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init_info.DescriptorPoolSize = 1000; // Let ImGui create its own pool

    if (!ImGui_ImplVulkan_Init(&init_info))
    {
        ImGui_ImplSDL2_Shutdown();
        return false;
    }

    return true;
}

#ifdef _WIN32
bool ImGuiManager::initD3D11(SDL_Window* window, IRenderAPI* d3d11API)
{
    // Initialize SDL2 backend for D3D11
    if (!ImGui_ImplSDL2_InitForD3D(window))
    {
        return false;
    }

    // Cast to D3D11RenderAPI to access D3D11 handles
    D3D11RenderAPI* dxAPI = dynamic_cast<D3D11RenderAPI*>(d3d11API);
    if (!dxAPI)
    {
        ImGui_ImplSDL2_Shutdown();
        return false;
    }

    if (!ImGui_ImplDX11_Init(dxAPI->getDevice(), dxAPI->getDeviceContext()))
    {
        ImGui_ImplSDL2_Shutdown();
        return false;
    }

    return true;
}
#endif

void ImGuiManager::shutdown()
{
    if (!m_initialized) return;

    if (m_apiType == RenderAPIType::OpenGL)
    {
        ImGui_ImplOpenGL3_Shutdown();
    }
    else if (m_apiType == RenderAPIType::Vulkan)
    {
        ImGui_ImplVulkan_Shutdown();
    }
#ifdef _WIN32
    else if (m_apiType == RenderAPIType::D3D11)
    {
        ImGui_ImplDX11_Shutdown();
    }
#endif

    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    m_initialized = false;
}

void ImGuiManager::newFrame()
{
    if (!m_initialized) return;

    // Start new ImGui frame - order matters!
    if (m_apiType == RenderAPIType::OpenGL)
    {
        ImGui_ImplOpenGL3_NewFrame();
    }
    else if (m_apiType == RenderAPIType::Vulkan)
    {
        ImGui_ImplVulkan_NewFrame();
    }
#ifdef _WIN32
    else if (m_apiType == RenderAPIType::D3D11)
    {
        ImGui_ImplDX11_NewFrame();
    }
#endif

    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
}

void ImGuiManager::render()
{
    if (!m_initialized) return;

    // FPS Counter overlay (top-left, non-interactive)
    ImGuiWindowFlags fps_flags = ImGuiWindowFlags_NoDecoration
        | ImGuiWindowFlags_AlwaysAutoResize
        | ImGuiWindowFlags_NoSavedSettings
        | ImGuiWindowFlags_NoFocusOnAppearing
        | ImGuiWindowFlags_NoNav
        | ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoInputs;

    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.35f);
    if (ImGui::Begin("FPS", nullptr, fps_flags))
    {
        ImGui::Text("%.1f FPS", ImGui::GetIO().Framerate);
    }
    ImGui::End();

    // Graphics Settings panel (only shown in UI mode - F3 to toggle)
    if (m_showSettings && m_renderAPI)
    {
        ImGui::SetNextWindowPos(ImVec2(10, 50), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(200, 0), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            if (ImGui::CollapsingHeader("Graphics", ImGuiTreeNodeFlags_DefaultOpen))
            {
                // FXAA toggle
                bool fxaa = m_renderAPI->isFXAAEnabled();
                if (ImGui::Checkbox("FXAA", &fxaa))
                {
                    m_renderAPI->setFXAAEnabled(fxaa);
                }

                // Shadow quality dropdown
                const char* shadowOptions[] = { "Off", "Low (1024)", "Medium (2048)", "High (4096)" };
                int shadowQuality = m_renderAPI->getShadowQuality();
                if (ImGui::Combo("Shadow Quality", &shadowQuality, shadowOptions, 4))
                {
                    m_renderAPI->setShadowQuality(shadowQuality);
                }
            }
        }
        ImGui::End();
    }

    // Finalize ImGui rendering - builds the draw data
    // Actual rendering to screen happens in each RenderAPI's endFrame() AFTER FXAA
    ImGui::Render();
}

bool ImGuiManager::processEvent(const SDL_Event* event)
{
    if (!m_initialized) return false;
    return ImGui_ImplSDL2_ProcessEvent(event);
}

bool ImGuiManager::wantCaptureMouse() const
{
    if (!m_initialized) return false;
    return ImGui::GetIO().WantCaptureMouse;
}

bool ImGuiManager::wantCaptureKeyboard() const
{
    if (!m_initialized) return false;
    return ImGui::GetIO().WantCaptureKeyboard;
}
