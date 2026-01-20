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
#include "Console/Console.hpp"
#include "Console/ConVar.hpp"
#include "Console/ConCommand.hpp"
#include <SDL.h>
#include <cstring>

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
