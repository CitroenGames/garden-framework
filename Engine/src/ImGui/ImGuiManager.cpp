#include "ImGuiManager.hpp"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"
#ifdef __APPLE__
#include "Graphics/MetalRenderAPI.hpp"
// Note: Metal ImGui backend calls (init/shutdown/newframe/render) use Objective-C types
// and are handled through helper functions in MetalRenderAPI.mm
extern "C" bool ImGuiMetal_Init(void* device);
extern "C" void ImGuiMetal_Shutdown();
extern "C" void ImGuiMetal_NewFrame(void* renderPassDescriptor);
#endif
#ifdef _WIN32
#include "imgui_impl_dx11.h"
#include "imgui_impl_dx12.h"
#include "Graphics/D3D11RenderAPI.hpp"
#include "Graphics/D3D12RenderAPI.hpp"
#endif
#include "Graphics/VulkanRenderAPI.hpp"
#include "Console/Console.hpp"
#include "Console/ConVar.hpp"
#include "Console/ConCommand.hpp"
#include "Utils/EnginePaths.hpp"
#include <SDL3/SDL.h>
#include <cstring>

ImGuiManager& ImGuiManager::get()
{
    static ImGuiManager instance;
    return instance;
}

static void ApplyEditorTheme()
{
    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();

    // Rounding — flat / angular (UE5 signature)
    style.WindowRounding    = 0.0f;
    style.ChildRounding     = 0.0f;
    style.FrameRounding     = 1.0f;
    style.PopupRounding     = 1.0f;
    style.ScrollbarRounding = 2.0f;
    style.GrabRounding      = 1.0f;
    style.TabRounding       = 0.0f;

    // Sizing — compact
    style.WindowPadding     = ImVec2(6.0f, 6.0f);
    style.FramePadding      = ImVec2(5.0f, 3.0f);
    style.CellPadding       = ImVec2(4.0f, 2.0f);
    style.ItemSpacing       = ImVec2(6.0f, 3.0f);
    style.ItemInnerSpacing  = ImVec2(4.0f, 3.0f);
    style.IndentSpacing     = 16.0f;
    style.ScrollbarSize     = 8.0f;
    style.GrabMinSize       = 8.0f;

    // Borders — minimal (UE5 signature)
    style.WindowBorderSize      = 0.0f;
    style.ChildBorderSize       = 0.0f;
    style.FrameBorderSize       = 0.0f;
    style.TabBorderSize         = 0.0f;
    style.TabBarBorderSize      = 1.0f;
    style.TabBarOverlineSize    = 1.5f;
    style.DockingSeparatorSize  = 1.0f;

    // Colors — UE5 neutral-grey palette with blue accents
    ImVec4* c = style.Colors;

    // Text
    c[ImGuiCol_Text]                  = ImVec4(0.88f, 0.88f, 0.88f, 1.00f);
    c[ImGuiCol_TextDisabled]          = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);

    // Backgrounds — neutral dark grey
    c[ImGuiCol_WindowBg]              = ImVec4(0.12f, 0.12f, 0.12f, 1.00f);
    c[ImGuiCol_ChildBg]               = ImVec4(0.12f, 0.12f, 0.12f, 1.00f);
    c[ImGuiCol_PopupBg]               = ImVec4(0.15f, 0.15f, 0.15f, 0.96f);

    // Borders — subtle neutral
    c[ImGuiCol_Border]                = ImVec4(0.17f, 0.17f, 0.17f, 0.40f);
    c[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    // Frames — darker neutral inset
    c[ImGuiCol_FrameBg]               = ImVec4(0.07f, 0.07f, 0.07f, 1.00f);
    c[ImGuiCol_FrameBgHovered]        = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);
    c[ImGuiCol_FrameBgActive]         = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);

    // Title bar — dark
    c[ImGuiCol_TitleBg]               = ImVec4(0.08f, 0.08f, 0.08f, 1.00f);
    c[ImGuiCol_TitleBgActive]         = ImVec4(0.08f, 0.08f, 0.08f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.08f, 0.08f, 0.08f, 0.75f);

    // Menu bar
    c[ImGuiCol_MenuBarBg]             = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);

    // Scrollbar — thin and unobtrusive
    c[ImGuiCol_ScrollbarBg]           = ImVec4(0.05f, 0.05f, 0.05f, 0.60f);
    c[ImGuiCol_ScrollbarGrab]         = ImVec4(0.28f, 0.28f, 0.28f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.36f, 0.36f, 0.36f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.44f, 0.44f, 0.44f, 1.00f);

    // Checkmark, slider grab — bright UE5 blue accent
    c[ImGuiCol_CheckMark]             = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    c[ImGuiCol_SliderGrab]            = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    c[ImGuiCol_SliderGrabActive]      = ImVec4(0.40f, 0.68f, 1.00f, 1.00f);

    // Buttons — neutral grey
    c[ImGuiCol_Button]                = ImVec4(0.19f, 0.19f, 0.19f, 1.00f);
    c[ImGuiCol_ButtonHovered]         = ImVec4(0.26f, 0.26f, 0.26f, 1.00f);
    c[ImGuiCol_ButtonActive]          = ImVec4(0.22f, 0.42f, 0.70f, 1.00f);

    // Headers — neutral grey, blue on active
    c[ImGuiCol_Header]                = ImVec4(0.19f, 0.19f, 0.19f, 1.00f);
    c[ImGuiCol_HeaderHovered]         = ImVec4(0.26f, 0.26f, 0.26f, 1.00f);
    c[ImGuiCol_HeaderActive]          = ImVec4(0.22f, 0.42f, 0.70f, 1.00f);

    // Separator — neutral
    c[ImGuiCol_Separator]             = ImVec4(0.19f, 0.19f, 0.19f, 1.00f);
    c[ImGuiCol_SeparatorHovered]      = ImVec4(0.26f, 0.59f, 0.98f, 0.78f);
    c[ImGuiCol_SeparatorActive]       = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);

    // Resize grip
    c[ImGuiCol_ResizeGrip]            = ImVec4(0.26f, 0.59f, 0.98f, 0.25f);
    c[ImGuiCol_ResizeGripHovered]     = ImVec4(0.26f, 0.59f, 0.98f, 0.67f);
    c[ImGuiCol_ResizeGripActive]      = ImVec4(0.26f, 0.59f, 0.98f, 0.95f);

    // Tabs — selected merges seamlessly with panel body (match WindowBg)
    c[ImGuiCol_Tab]                   = ImVec4(0.08f, 0.08f, 0.08f, 1.00f);
    c[ImGuiCol_TabHovered]            = ImVec4(0.17f, 0.17f, 0.17f, 1.00f);
    c[ImGuiCol_TabSelected]           = ImVec4(0.12f, 0.12f, 0.12f, 1.00f);
    c[ImGuiCol_TabSelectedOverline]   = ImVec4(0.10f, 0.46f, 0.82f, 1.00f);
    c[ImGuiCol_TabDimmed]             = ImVec4(0.07f, 0.07f, 0.07f, 1.00f);
    c[ImGuiCol_TabDimmedSelected]     = ImVec4(0.11f, 0.11f, 0.11f, 1.00f);
    c[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0.10f, 0.36f, 0.65f, 1.00f);

    // Docking
    c[ImGuiCol_DockingPreview]        = ImVec4(0.26f, 0.59f, 0.98f, 0.70f);
    c[ImGuiCol_DockingEmptyBg]        = ImVec4(0.06f, 0.06f, 0.06f, 1.00f);

    // Tables
    c[ImGuiCol_TableHeaderBg]         = ImVec4(0.12f, 0.12f, 0.12f, 1.00f);
    c[ImGuiCol_TableBorderStrong]     = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    c[ImGuiCol_TableBorderLight]      = ImVec4(0.17f, 0.17f, 0.17f, 1.00f);
    c[ImGuiCol_TableRowBg]            = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_TableRowBgAlt]         = ImVec4(1.00f, 1.00f, 1.00f, 0.03f);

    // Misc
    c[ImGuiCol_DragDropTarget]        = ImVec4(0.26f, 0.59f, 0.98f, 0.90f);
    c[ImGuiCol_NavCursor]             = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    c[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    c[ImGuiCol_NavWindowingDimBg]     = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
    c[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.00f, 0.00f, 0.00f, 0.55f);
}

void ImGuiManager::applyTheme() { ApplyEditorTheme(); }

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
    // Multi-viewport: allows ImGui windows to be dragged to separate OS windows (second monitor)
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

    // Setup style
    ApplyEditorTheme();

    // Load sans-serif font for UE5-like appearance + merge Font Awesome icons
    {
        std::string fontPath = EnginePaths::resolveEngineAsset("../assets/fonts/LatoLatin-Regular.ttf");
        std::string iconFontPath = EnginePaths::resolveEngineAsset("../assets/fonts/fa-solid-900.ttf");
        static const ImWchar icons_ranges[] = { 0xF000, 0xF900, 0 };

        // Regular font
        ImFont* font = io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 15.0f);
        if (!font)
            io.Fonts->AddFontDefault();

        // Merge FA icons into regular font
        {
            ImFontConfig icons_config;
            icons_config.MergeMode = true;
            icons_config.PixelSnapH = true;
            icons_config.GlyphMinAdvanceX = 15.0f;
            io.Fonts->AddFontFromFileTTF(iconFontPath.c_str(), 14.0f, &icons_config, icons_ranges);
        }

        // Bold font for section headers
        std::string boldFontPath = EnginePaths::resolveEngineAsset("../assets/fonts/LatoLatin-Bold.ttf");
        m_boldFont = io.Fonts->AddFontFromFileTTF(boldFontPath.c_str(), 15.0f);
        if (!m_boldFont)
            m_boldFont = io.Fonts->Fonts[0];

        // Merge FA icons into bold font
        {
            ImFontConfig icons_config;
            icons_config.MergeMode = true;
            icons_config.PixelSnapH = true;
            icons_config.GlyphMinAdvanceX = 15.0f;
            io.Fonts->AddFontFromFileTTF(iconFontPath.c_str(), 14.0f, &icons_config, icons_ranges);
        }

        // Larger standalone icon font for toolbar buttons and content browser grid
        m_iconFont = io.Fonts->AddFontFromFileTTF(iconFontPath.c_str(), 20.0f, nullptr, icons_ranges);
    }

    // Initialize platform and renderer backends
    bool success = false;
    if (apiType == RenderAPIType::Vulkan)
    {
        success = initVulkan(window, renderAPI);
    }
#ifdef __APPLE__
    else if (apiType == RenderAPIType::Metal)
    {
        success = initMetal(window, renderAPI);
    }
#endif
#ifdef _WIN32
    else if (apiType == RenderAPIType::D3D11)
    {
        success = initD3D11(window, renderAPI);
    }
    else if (apiType == RenderAPIType::D3D12)
    {
        success = initD3D12(window, renderAPI);
    }
#endif

    m_initialized = success;
    return success;
}

bool ImGuiManager::initVulkan(SDL_Window* window, IRenderAPI* vulkanAPI)
{
    // Initialize SDL3 backend for Vulkan
    if (!ImGui_ImplSDL3_InitForVulkan(window))
    {
        return false;
    }

    // Cast to VulkanRenderAPI to access Vulkan handles
    VulkanRenderAPI* vkAPI = dynamic_cast<VulkanRenderAPI*>(vulkanAPI);
    if (!vkAPI)
    {
        ImGui_ImplSDL3_Shutdown();
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
        ImGui_ImplSDL3_Shutdown();
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
    init_info.PipelineInfoMain.RenderPass = renderPass;
    init_info.MinImageCount = 2;
    init_info.ImageCount = vkAPI->getSwapchainImageCount();
    init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init_info.DescriptorPoolSize = 1000; // Let ImGui create its own pool

    if (!ImGui_ImplVulkan_Init(&init_info))
    {
        ImGui_ImplSDL3_Shutdown();
        return false;
    }

    return true;
}

#ifdef __APPLE__
bool ImGuiManager::initMetal(SDL_Window* window, IRenderAPI* metalAPI)
{
    // Initialize SDL3 backend for Metal
    if (!ImGui_ImplSDL3_InitForMetal(window))
    {
        return false;
    }

    MetalRenderAPI* mtlAPI = dynamic_cast<MetalRenderAPI*>(metalAPI);
    if (!mtlAPI)
    {
        ImGui_ImplSDL3_Shutdown();
        return false;
    }

    void* device = mtlAPI->getDevice();
    if (!device)
    {
        ImGui_ImplSDL3_Shutdown();
        return false;
    }

    if (!ImGuiMetal_Init(device))
    {
        ImGui_ImplSDL3_Shutdown();
        return false;
    }

    return true;
}
#endif

#ifdef _WIN32

// Forward declare (intentionally not in imgui_impl_dx11.h per ImGui docs)
void ImGui_ImplDX11_SetSwapChainDescs(const DXGI_SWAP_CHAIN_DESC* desc_templates, int desc_templates_count);

bool ImGuiManager::initD3D11(SDL_Window* window, IRenderAPI* d3d11API)
{
    // Initialize SDL3 backend for D3D11
    if (!ImGui_ImplSDL3_InitForD3D(window))
    {
        return false;
    }

    // Cast to D3D11RenderAPI to access D3D11 handles
    D3D11RenderAPI* dxAPI = dynamic_cast<D3D11RenderAPI*>(d3d11API);
    if (!dxAPI)
    {
        ImGui_ImplSDL3_Shutdown();
        return false;
    }

    if (!ImGui_ImplDX11_Init(dxAPI->getDevice(), dxAPI->getDeviceContext()))
    {
        ImGui_ImplSDL3_Shutdown();
        return false;
    }

    // Match secondary viewport swap chains to the engine's main swap chain settings.
    // Without this, ImGui defaults to DXGI_SWAP_EFFECT_DISCARD which conflicts with
    // the engine's DXGI_SWAP_EFFECT_FLIP_DISCARD, causing black viewports when
    // dragging panels out (see ocornut/imgui#5437).
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ImGui_ImplDX11_SetSwapChainDescs(&sd, 1);

    return true;
}

bool ImGuiManager::initD3D12(SDL_Window* window, IRenderAPI* d3d12API)
{
    if (!ImGui_ImplSDL3_InitForD3D(window))
    {
        return false;
    }

    D3D12RenderAPI* dxAPI = dynamic_cast<D3D12RenderAPI*>(d3d12API);
    if (!dxAPI)
    {
        ImGui_ImplSDL3_Shutdown();
        return false;
    }

    ImGui_ImplDX12_InitInfo init_info = {};
    init_info.Device = dxAPI->getDevice();
    init_info.CommandQueue = dxAPI->getCommandQueue();
    init_info.NumFramesInFlight = D3D12RenderAPI::NUM_FRAMES_IN_FLIGHT;
    init_info.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    init_info.DSVFormat = DXGI_FORMAT_UNKNOWN;
    init_info.SrvDescriptorHeap = dxAPI->getSrvDescriptorHeap();

    // Use ImGui's descriptor allocation callbacks via the engine's allocator
    init_info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu, D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu)
    {
        D3D12RenderAPI* api = static_cast<D3D12RenderAPI*>(info->UserData);
        auto& alloc = api->getSrvAllocator();
        UINT idx = alloc.allocate();
        *out_cpu = alloc.getCPU(idx);
        *out_gpu = alloc.getGPU(idx);
    };
    init_info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE gpu)
    {
        D3D12RenderAPI* api = static_cast<D3D12RenderAPI*>(info->UserData);
        auto& alloc = api->getSrvAllocator();
        // Calculate index from CPU handle
        UINT idx = static_cast<UINT>((cpu.ptr - alloc.getCPU(0).ptr) / alloc.descriptorSize);
        alloc.free(idx);
    };
    init_info.UserData = dxAPI;

    if (!ImGui_ImplDX12_Init(&init_info))
    {
        ImGui_ImplSDL3_Shutdown();
        return false;
    }

    return true;
}
#endif

void ImGuiManager::shutdown()
{
    if (!m_initialized) return;

    if (m_apiType == RenderAPIType::Vulkan)
    {
        ImGui_ImplVulkan_Shutdown();
    }
#ifdef __APPLE__
    else if (m_apiType == RenderAPIType::Metal)
    {
        ImGuiMetal_Shutdown();
    }
#endif
#ifdef _WIN32
    else if (m_apiType == RenderAPIType::D3D11)
    {
        ImGui_ImplDX11_Shutdown();
    }
    else if (m_apiType == RenderAPIType::D3D12)
    {
        ImGui_ImplDX12_Shutdown();
    }
#endif

    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    m_initialized = false;
}

void ImGuiManager::newFrame()
{
    if (!m_initialized) return;

    // Start new ImGui frame - order matters!
    if (m_apiType == RenderAPIType::Vulkan)
    {
        ImGui_ImplVulkan_NewFrame();
    }
#ifdef __APPLE__
    else if (m_apiType == RenderAPIType::Metal)
    {
        // Metal NewFrame is called from MetalRenderAPI::beginFrame() with a valid render pass descriptor
    }
#endif
#ifdef _WIN32
    else if (m_apiType == RenderAPIType::D3D11)
    {
        ImGui_ImplDX11_NewFrame();
    }
    else if (m_apiType == RenderAPIType::D3D12)
    {
        ImGui_ImplDX12_NewFrame();
    }
#endif

    ImGui_ImplSDL3_NewFrame();
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

    // Console window
    renderConsole();

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
                    if (auto* cvar = CVAR_PTR(r_fxaa))
                        cvar->setInt(fxaa ? 1 : 0);
                }

                // Shadow quality dropdown
                const char* shadowOptions[] = { "Off", "Low (1024)", "Medium (2048)", "High (4096)" };
                int shadowQuality = m_renderAPI->getShadowQuality();
                if (ImGui::Combo("Shadow Quality", &shadowQuality, shadowOptions, 4))
                {
                    m_renderAPI->setShadowQuality(shadowQuality);
                    if (auto* cvar = CVAR_PTR(r_shadowquality))
                        cvar->setInt(shadowQuality);
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
    return ImGui_ImplSDL3_ProcessEvent(event);
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

void ImGuiManager::renderConsole()
{
    if (!m_showConsole) return;

    ImGui::SetNextWindowSize(ImVec2(800, 400), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(50, 50), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("Console", &m_showConsole, ImGuiWindowFlags_NoCollapse))
    {
        // Filter dropdown and clear button
        const char* filterItems[] = { "All", "Warnings+", "Errors Only" };
        ImGui::SetNextItemWidth(120);
        ImGui::Combo("##Filter", &m_logLevelFilter, filterItems, 3);
        ImGui::SameLine();
        if (ImGui::Button("Clear"))
        {
            Console::get().clear();
        }
        ImGui::Separator();

        // Log output area
        float footerHeight = ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();
        ImGui::BeginChild("LogRegion", ImVec2(0, -footerHeight), true, ImGuiWindowFlags_HorizontalScrollbar);

        const auto& entries = Console::get().getLogEntries();
        for (const auto& entry : entries)
        {
            // Filter by level
            if (m_logLevelFilter == 1 && entry.level < spdlog::level::warn) continue;
            if (m_logLevelFilter == 2 && entry.level < spdlog::level::err) continue;

            // Color by level
            ImVec4 color;
            switch (entry.level)
            {
            case spdlog::level::err:
            case spdlog::level::critical:
                color = ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
                break;
            case spdlog::level::warn:
                color = ImVec4(1.0f, 0.8f, 0.4f, 1.0f);
                break;
            case spdlog::level::info:
                color = ImVec4(0.9f, 0.9f, 0.9f, 1.0f);
                break;
            default:
                color = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
                break;
            }

            ImGui::PushStyleColor(ImGuiCol_Text, color);
            ImGui::TextUnformatted(entry.message.c_str());
            ImGui::PopStyleColor();
        }

        if (m_scrollToBottom)
        {
            ImGui::SetScrollHereY(1.0f);
            m_scrollToBottom = false;
        }

        ImGui::EndChild();

        // Input field
        ImGui::Separator();

        bool reclaimFocus = false;
        ImGuiInputTextFlags inputFlags = ImGuiInputTextFlags_EnterReturnsTrue |
                                         ImGuiInputTextFlags_CallbackHistory |
                                         ImGuiInputTextFlags_CallbackCompletion |
                                         ImGuiInputTextFlags_CallbackEdit;

        ImGui::SetNextItemWidth(-1);
        bool inputSubmitted = ImGui::InputText("##ConsoleInput", m_consoleInput, sizeof(m_consoleInput),
                             inputFlags, &ImGuiManager::consoleInputCallback, this);

        // Get input field position for autocomplete popup
        ImVec2 inputPos = ImGui::GetItemRectMin();
        float inputWidth = ImGui::GetItemRectSize().x;

        // Handle autocomplete selection with arrow keys when popup is visible
        if (m_showAutocomplete && !m_autocompleteItems.empty())
        {
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
            {
                if (m_autocompleteSelectedIndex > 0)
                {
                    m_autocompleteSelectedIndex--;
                }
            }
            else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
            {
                if (m_autocompleteSelectedIndex < static_cast<int>(m_autocompleteItems.size()) - 1)
                {
                    m_autocompleteSelectedIndex++;
                }
            }
            else if (ImGui::IsKeyPressed(ImGuiKey_Tab) || ImGui::IsKeyPressed(ImGuiKey_Enter))
            {
                if (m_autocompleteSelectedIndex >= 0 && m_autocompleteSelectedIndex < static_cast<int>(m_autocompleteItems.size()))
                {
                    // Apply selected completion
                    std::strncpy(m_consoleInput, m_autocompleteItems[m_autocompleteSelectedIndex].c_str(), sizeof(m_consoleInput) - 1);
                    std::strncat(m_consoleInput, " ", sizeof(m_consoleInput) - strlen(m_consoleInput) - 1);
                    m_showAutocomplete = false;
                    m_autocompleteItems.clear();
                    m_autocompleteSelectedIndex = -1;
                    reclaimFocus = true;

                    // If Enter was pressed on a selection, don't submit the command
                    if (ImGui::IsKeyPressed(ImGuiKey_Enter))
                    {
                        inputSubmitted = false;
                    }
                }
            }
            else if (ImGui::IsKeyPressed(ImGuiKey_Escape))
            {
                m_showAutocomplete = false;
                m_autocompleteItems.clear();
                m_autocompleteSelectedIndex = -1;
            }
        }

        if (inputSubmitted)
        {
            std::string cmd(m_consoleInput);
            if (!cmd.empty())
            {
                Console::get().submitCommand(cmd);
                m_scrollToBottom = true;
            }
            m_consoleInput[0] = '\0';
            reclaimFocus = true;
            m_historyIndex = -1;
            m_showAutocomplete = false;
            m_autocompleteItems.clear();
            m_autocompleteSelectedIndex = -1;
        }

        // Autocomplete popup
        if (m_showAutocomplete && !m_autocompleteItems.empty())
        {
            ImGui::SetNextWindowPos(ImVec2(inputPos.x, inputPos.y + ImGui::GetFrameHeight()));
            ImGui::SetNextWindowSize(ImVec2(inputWidth, 0));

            ImGuiWindowFlags popupFlags = ImGuiWindowFlags_NoTitleBar |
                                          ImGuiWindowFlags_NoMove |
                                          ImGuiWindowFlags_NoResize |
                                          ImGuiWindowFlags_NoSavedSettings |
                                          ImGuiWindowFlags_NoFocusOnAppearing;

            ImGui::Begin("##AutocompletePopup", nullptr, popupFlags);

            for (int i = 0; i < static_cast<int>(m_autocompleteItems.size()) && i < 10; i++)
            {
                bool isSelected = (i == m_autocompleteSelectedIndex);
                if (ImGui::Selectable(m_autocompleteItems[i].c_str(), isSelected))
                {
                    std::strncpy(m_consoleInput, m_autocompleteItems[i].c_str(), sizeof(m_consoleInput) - 1);
                    std::strncat(m_consoleInput, " ", sizeof(m_consoleInput) - strlen(m_consoleInput) - 1);
                    m_showAutocomplete = false;
                    m_autocompleteItems.clear();
                    m_autocompleteSelectedIndex = -1;
                    reclaimFocus = true;
                }
            }

            if (m_autocompleteItems.size() > 10)
            {
                ImGui::TextDisabled("... and %d more", static_cast<int>(m_autocompleteItems.size()) - 10);
            }

            ImGui::End();
        }

        // Auto-focus on input when console opens
        if (ImGui::IsWindowAppearing())
        {
            ImGui::SetKeyboardFocusHere(-1);
        }

        if (reclaimFocus)
        {
            ImGui::SetKeyboardFocusHere(-1);
        }
    }
    ImGui::End();
}

int ImGuiManager::consoleInputCallback(ImGuiInputTextCallbackData* data)
{
    ImGuiManager* manager = static_cast<ImGuiManager*>(data->UserData);

    switch (data->EventFlag)
    {
    case ImGuiInputTextFlags_CallbackHistory:
    {
        const int prevIndex = manager->m_historyIndex;

        if (data->EventKey == ImGuiKey_UpArrow)
        {
            if (manager->m_historyIndex < Console::get().getHistoryCount() - 1)
            {
                manager->m_historyIndex++;
            }
        }
        else if (data->EventKey == ImGuiKey_DownArrow)
        {
            if (manager->m_historyIndex > 0)
            {
                manager->m_historyIndex--;
            }
            else
            {
                manager->m_historyIndex = -1;
            }
        }

        if (prevIndex != manager->m_historyIndex)
        {
            if (manager->m_historyIndex >= 0)
            {
                const std::string& history = Console::get().getHistoryItem(manager->m_historyIndex);
                data->DeleteChars(0, data->BufTextLen);
                data->InsertChars(0, history.c_str());
            }
            else
            {
                data->DeleteChars(0, data->BufTextLen);
            }
        }
        break;
    }

    case ImGuiInputTextFlags_CallbackCompletion:
    {
        // Tab completion - apply selected or first completion
        if (manager->m_showAutocomplete && !manager->m_autocompleteItems.empty())
        {
            int idx = manager->m_autocompleteSelectedIndex >= 0 ? manager->m_autocompleteSelectedIndex : 0;
            if (idx < static_cast<int>(manager->m_autocompleteItems.size()))
            {
                data->DeleteChars(0, data->BufTextLen);
                data->InsertChars(0, manager->m_autocompleteItems[idx].c_str());
                data->InsertChars(data->CursorPos, " ");
                manager->m_showAutocomplete = false;
                manager->m_autocompleteItems.clear();
                manager->m_autocompleteSelectedIndex = -1;
            }
        }
        else
        {
            std::string partial(data->Buf, data->CursorPos);
            auto completions = Console::get().getCompletions(partial);

            if (completions.size() == 1)
            {
                data->DeleteChars(0, data->BufTextLen);
                data->InsertChars(0, completions[0].c_str());
                data->InsertChars(data->CursorPos, " ");
            }
            else if (completions.size() > 1)
            {
                // Find common prefix
                std::string prefix = completions[0];
                for (size_t i = 1; i < completions.size(); i++)
                {
                    size_t j = 0;
                    while (j < prefix.size() && j < completions[i].size() &&
                           prefix[j] == completions[i][j])
                    {
                        j++;
                    }
                    prefix = prefix.substr(0, j);
                }

                if (prefix.size() > partial.size())
                {
                    data->DeleteChars(0, data->BufTextLen);
                    data->InsertChars(0, prefix.c_str());
                }
            }
        }
        break;
    }

    case ImGuiInputTextFlags_CallbackEdit:
    {
        // Update autocomplete as user types
        std::string partial(data->Buf, data->BufTextLen);

        if (partial.length() >= 2)
        {
            manager->m_autocompleteItems = Console::get().getCompletions(partial);
            manager->m_showAutocomplete = !manager->m_autocompleteItems.empty();
            manager->m_autocompleteSelectedIndex = manager->m_showAutocomplete ? 0 : -1;
        }
        else
        {
            manager->m_showAutocomplete = false;
            manager->m_autocompleteItems.clear();
            manager->m_autocompleteSelectedIndex = -1;
        }
        break;
    }
    }

    return 0;
}
