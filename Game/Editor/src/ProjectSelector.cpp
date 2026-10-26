#include "ProjectSelector.hpp"
#include "Project/ProjectManager.hpp"
#include "Utils/EnginePaths.hpp"
#include "Utils/FileDialog.hpp"

#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace
{
fs::path findFontsDir()
{
    const fs::path exe_dir = EnginePaths::getExecutableDir();
    std::error_code ec;
    const fs::path cwd = fs::current_path(ec);
    const std::vector<fs::path> candidates = {
        exe_dir / ".." / "assets" / "fonts",
        cwd / "assets" / "fonts",
        cwd / ".." / "assets" / "fonts",
    };
    for (const fs::path& dir : candidates)
    {
        std::error_code error;
        if (fs::exists(dir / "LatoLatin-Regular.ttf", error) && !error)
            return dir;
    }
    return {};
}
} // namespace

std::string ProjectSelector::run()
{
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
    {
        fprintf(stderr, "[ProjectSelector] SDL_Init failed: %s\n", SDL_GetError());
        return "";
    }

    SDL_Window* window = SDL_CreateWindow(
        "Garden - Project Browser", 1040, 700,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window)
    {
        fprintf(stderr, "[ProjectSelector] SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return "";
    }
    SDL_SetWindowMinimumSize(window, 1040, 700);
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);

    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer)
    {
        fprintf(stderr, "[ProjectSelector] SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return "";
    }
    SDL_SetRenderVSync(renderer, 1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    const fs::path fonts_dir = findFontsDir();
    bool font_loaded = false;
    if (!fonts_dir.empty())
    {
        const std::string regular = (fonts_dir / "LatoLatin-Regular.ttf").generic_string();
        const std::string bold = (fonts_dir / "LatoLatin-Bold.ttf").generic_string();
        font_loaded = io.Fonts->AddFontFromFileTTF(regular.c_str(), 16.0f) != nullptr;
        std::error_code error;
        if (fs::exists(bold, error) && !error)
            io.Fonts->AddFontFromFileTTF(bold.c_str(), 16.0f);
    }
    if (!font_loaded)
    {
        fprintf(stderr, "[ProjectSelector] Warning: failed to locate fonts.\n");
        io.Fonts->AddFontDefault();
    }

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.FrameRounding = 3.0f;
    style.WindowPadding = ImVec2(16.0f, 12.0f);
    style.ItemSpacing = ImVec2(8.0f, 8.0f);
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.09f, 0.10f, 0.11f, 1.0f);
    style.Colors[ImGuiCol_Button] = ImVec4(0.15f, 0.19f, 0.23f, 1.0f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.19f, 0.25f, 0.30f, 1.0f);
    style.Colors[ImGuiCol_Header] = ImVec4(0.11f, 0.23f, 0.27f, 1.0f);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.14f, 0.30f, 0.35f, 1.0f);
    style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.16f, 0.35f, 0.40f, 1.0f);

    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    const fs::path exe_dir = EnginePaths::getExecutableDir();
    const fs::path templates_dir = exe_dir / ".." / "Templates";
    std::vector<TemplateInfo> templates = ProjectManager::discoverTemplates(templates_dir.string());
    ProjectManager project_manager;

    std::string result_path;
    bool done = false;
    int active_tab = 0; // 0 = Project Browser (create), 1 = Open Existing
    int selected_template = templates.empty() ? -1 : 0;
    char name_buf[256] = "MyGame";
    char dir_buf[1024] = ".";
    char open_buf[1024] = "";
    std::string status;
    bool status_error = false;

    auto can_create = [&]() {
        return selected_template >= 0 && selected_template < static_cast<int>(templates.size())
            && name_buf[0] != '\0' && dir_buf[0] != '\0';
    };
    auto primary_button = [&](const char* label, bool enabled, ImVec2 size) {
        if (!enabled)
            ImGui::BeginDisabled();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.17f, 0.47f, 0.53f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.21f, 0.55f, 0.61f, 1.0f));
        bool pressed = ImGui::Button(label, size);
        ImGui::PopStyleColor(2);
        if (!enabled)
            ImGui::EndDisabled();
        return pressed && enabled;
    };

    while (!done)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT)
                done = true;
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                event.window.windowID == SDL_GetWindowID(window))
                done = true;
            if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE)
                done = true;
        }
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED)
        {
            SDL_Delay(10);
            continue;
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->Pos);
        ImGui::SetNextWindowSize(viewport->Size);
        ImGui::Begin("##ProjectSelector", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

        ImGui::TextUnformatted("Garden Engine");
        ImGui::SameLine(0, 24);
        if (ImGui::Selectable("Project Browser", active_tab == 0, 0, ImVec2(130, 28)))
        {
            active_tab = 0;
            status.clear();
        }
        ImGui::SameLine();
        if (ImGui::Selectable("Open Existing Project", active_tab == 1, 0, ImVec2(180, 28)))
        {
            active_tab = 1;
            status.clear();
        }
        ImGui::Separator();

        const float side_w = 280.0f;
        if (active_tab == 0)
        {
            ImGui::BeginChild("##create_form", ImVec2(-side_w - 16.0f, 0), ImGuiChildFlags_Borders);
            ImGui::TextUnformatted("Create Project");
            ImGui::TextWrapped("Choose a template and project location. The editor opens the new .garden project after creation.");
            ImGui::Spacing();
            ImGui::TextUnformatted("Project name");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##new_project_name", name_buf, sizeof(name_buf));
            ImGui::Spacing();
            ImGui::TextUnformatted("Project directory");
            const float browse_w = 108.0f;
            ImGui::SetNextItemWidth(-browse_w - 8.0f);
            ImGui::InputText("##new_project_dir", dir_buf, sizeof(dir_buf));
            ImGui::SameLine();
            if (ImGui::Button("Browse", ImVec2(browse_w, 0)))
            {
                std::string folder = FileDialog::openFolder("Select Project Directory");
                if (!folder.empty())
                {
                    std::snprintf(dir_buf, sizeof(dir_buf), "%s", folder.c_str());
                    status.clear();
                }
            }
            ImGui::Spacing();
            const bool enabled = can_create();
            if (primary_button("Create Project", enabled, ImVec2(184, 40)))
            {
                if (project_manager.createProjectFromTemplate(
                        templates[selected_template].path, dir_buf, name_buf))
                {
                    result_path = project_manager.getProjectFilePath();
                    done = true;
                }
                else
                {
                    status = "Failed to create the project. Check the directory and template files.";
                    status_error = true;
                    fprintf(stderr, "[ProjectSelector] Failed to create project '%s' in '%s'\n",
                            name_buf, dir_buf);
                }
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", enabled ? "Ready to create." : "Name, directory, and template are required.");
            ImGui::EndChild();

            ImGui::SameLine();
            ImGui::BeginChild("##templates", ImVec2(side_w, 0), ImGuiChildFlags_Borders);
            ImGui::TextDisabled("TEMPLATES");
            ImGui::Separator();
            if (templates.empty())
            {
                ImGui::TextWrapped("No templates were found in the Templates directory.");
            }
            else
            {
                ImGui::BeginChild("##template_list", ImVec2(0, -110), 0);
                for (int i = 0; i < static_cast<int>(templates.size()); ++i)
                {
                    if (ImGui::Selectable(templates[i].name.c_str(), i == selected_template,
                                          ImGuiSelectableFlags_None, ImVec2(0, 36)))
                        selected_template = i;
                }
                ImGui::EndChild();
            }
            ImGui::Separator();
            ImGui::TextDisabled("Selected template");
            if (selected_template >= 0 && selected_template < static_cast<int>(templates.size()))
            {
                ImGui::TextUnformatted(templates[selected_template].name.c_str());
                ImGui::TextDisabled("Path");
                ImGui::TextWrapped("%s", templates[selected_template].path.c_str());
            }
            else
            {
                ImGui::TextUnformatted("None");
                ImGui::TextWrapped("No template selected.");
            }
            ImGui::EndChild();
        }
        else
        {
            ImGui::BeginChild("##open_form", ImVec2(-side_w - 16.0f, 0), ImGuiChildFlags_Borders);
            ImGui::TextUnformatted("Open Project");
            ImGui::TextWrapped("Select an existing .garden project file. The editor will load it directly.");
            ImGui::Spacing();
            ImGui::TextUnformatted("Project file");
            const float browse_w = 108.0f;
            ImGui::SetNextItemWidth(-browse_w - 8.0f);
            ImGui::InputText("##open_project_path", open_buf, sizeof(open_buf));
            ImGui::SameLine();
            if (ImGui::Button("Browse", ImVec2(browse_w, 0)))
            {
                std::string file = FileDialog::openFile(
                    "Open Garden Project",
                    "Garden Project (*.garden)\0*.garden\0All Files (*.*)\0*.*\0");
                if (!file.empty())
                {
                    std::snprintf(open_buf, sizeof(open_buf), "%s", file.c_str());
                    status.clear();
                }
            }
            ImGui::Spacing();
            const bool enabled = open_buf[0] != '\0';
            if (primary_button("Open Project", enabled, ImVec2(184, 40)))
            {
                const fs::path path(open_buf);
                std::error_code exists_error;
                if (!fs::exists(path, exists_error) || exists_error)
                {
                    status = "That project file does not exist.";
                    status_error = true;
                }
                else
                {
                    std::error_code absolute_error;
                    fs::path absolute_path = fs::absolute(path, absolute_error);
                    if (absolute_error)
                    {
                        status = "Could not resolve that project path.";
                        status_error = true;
                    }
                    else
                    {
                        result_path = absolute_path.string();
                        done = true;
                    }
                }
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", enabled ? "Ready to open." : "Choose a .garden file first.");
            ImGui::EndChild();

            ImGui::SameLine();
            ImGui::BeginChild("##open_side", ImVec2(side_w, 0), ImGuiChildFlags_Borders);
            ImGui::TextDisabled("PROJECT FILE");
            ImGui::Separator();
            ImGui::TextWrapped("Garden project files use the .garden extension and describe the editor project root.");
            ImGui::Separator();
            ImGui::TextDisabled("Current path");
            ImGui::TextWrapped("%s", open_buf[0] ? open_buf : "No project selected.");
            ImGui::EndChild();
        }

        if (!status.empty())
        {
            ImGui::TextColored(status_error ? ImVec4(0.95f, 0.60f, 0.63f, 1.0f)
                                            : ImVec4(0.50f, 0.85f, 0.70f, 1.0f),
                               "%s", status.c_str());
        }

        ImGui::End();
        ImGui::Render();

        SDL_SetRenderDrawColor(renderer, 23, 26, 29, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return result_path;
}
