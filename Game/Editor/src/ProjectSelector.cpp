#include "ProjectSelector.hpp"
#include "Project/ProjectManager.hpp"
#include "Utils/FileDialog.hpp"
#include "Utils/EnginePaths.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include <filesystem>
#include <cstring>
#include <cstdio>
#include <system_error>

std::string ProjectSelector::run()
{
    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        fprintf(stderr, "[ProjectSelector] SDL_Init failed: %s\n", SDL_GetError());
        return "";
    }

    SDL_Window* window = SDL_CreateWindow(
        "Garden - Project Browser",
        900, 600,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);

    if (!window)
    {
        fprintf(stderr, "[ProjectSelector] SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return "";
    }
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);

    SDL_Renderer* renderer = SDL_CreateRenderer(window, NULL);

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
    ImGui::StyleColorsDark();
    {
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 0.0f;
        style.ChildRounding = 6.0f;
        style.FrameRounding = 4.0f;
        style.PopupRounding = 4.0f;
        style.GrabRounding = 4.0f;
        style.ScrollbarRounding = 6.0f;
        style.FramePadding = ImVec2(10.0f, 8.0f);
        style.ItemSpacing = ImVec2(10.0f, 10.0f);
        style.WindowPadding = ImVec2(0.0f, 0.0f);

        ImVec4* colors = style.Colors;
        colors[ImGuiCol_Text] = ImVec4(0.88f, 0.90f, 0.94f, 1.0f);
        colors[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.55f, 0.64f, 1.0f);
        colors[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.08f, 0.10f, 1.0f);
        colors[ImGuiCol_ChildBg] = ImVec4(0.09f, 0.10f, 0.13f, 1.0f);
        colors[ImGuiCol_Border] = ImVec4(0.20f, 0.25f, 0.32f, 1.0f);
        colors[ImGuiCol_FrameBg] = ImVec4(0.12f, 0.15f, 0.20f, 1.0f);
        colors[ImGuiCol_FrameBgHovered] = ImVec4(0.16f, 0.22f, 0.29f, 1.0f);
        colors[ImGuiCol_FrameBgActive] = ImVec4(0.18f, 0.27f, 0.36f, 1.0f);
        colors[ImGuiCol_Button] = ImVec4(0.14f, 0.18f, 0.24f, 1.0f);
        colors[ImGuiCol_ButtonHovered] = ImVec4(0.18f, 0.26f, 0.34f, 1.0f);
        colors[ImGuiCol_ButtonActive] = ImVec4(0.16f, 0.44f, 0.56f, 1.0f);
        colors[ImGuiCol_Header] = ImVec4(0.14f, 0.33f, 0.43f, 1.0f);
        colors[ImGuiCol_HeaderHovered] = ImVec4(0.18f, 0.42f, 0.54f, 1.0f);
        colors[ImGuiCol_HeaderActive] = ImVec4(0.20f, 0.50f, 0.62f, 1.0f);
    }

    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    // Discover available templates
    std::filesystem::path exe_dir = EnginePaths::getExecutableDir();
    std::filesystem::path templates_dir = exe_dir / ".." / "Templates";
    std::vector<TemplateInfo> available_templates =
        ProjectManager::discoverTemplates(templates_dir.string());
    int selected_template = 0;

    // UI state
    char new_project_name[256] = "MyGame";
    char new_project_dir[512]  = ".";
    char open_project_path[512] = "";

    enum class BrowserTab
    {
        ProjectBrowser,
        OpenExistingProject
    };
    BrowserTab active_tab = BrowserTab::ProjectBrowser;

    ProjectManager project_manager;
    std::string result_path;
    std::string status_message;
    bool status_is_error = false;
    bool running = true;

    while (running)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT)
                running = false;
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
                running = false;
        }

        if (!running)
            break;

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        // ---- Fullscreen background window ----
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.10f, 0.10f, 0.12f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

        ImGuiWindowFlags bg_flags = ImGuiWindowFlags_NoTitleBar |
                                    ImGuiWindowFlags_NoResize |
                                    ImGuiWindowFlags_NoMove |
                                    ImGuiWindowFlags_NoCollapse |
                                    ImGuiWindowFlags_NoDocking |
                                    ImGuiWindowFlags_NoBringToFrontOnFocus |
                                    ImGuiWindowFlags_NoNavFocus |
                                    ImGuiWindowFlags_NoScrollbar;

        ImGui::Begin("##ProjectBrowserBG", nullptr, bg_flags);

        // ---- Custom title bar ----
        {
            ImDrawList* draw = ImGui::GetWindowDrawList();
            float title_h = 48.0f;
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImVec2 p1(p0.x + io.DisplaySize.x, p0.y + title_h);

            draw->AddRectFilled(p0, p1, IM_COL32(22, 22, 26, 255));

            ImGui::PushFont(nullptr);
            const char* title = "Garden Engine";
            ImVec2 text_size = ImGui::CalcTextSize(title);
            ImVec2 text_pos(p0.x + 20.0f, p0.y + (title_h - text_size.y) * 0.5f);
            draw->AddText(text_pos, IM_COL32(200, 200, 200, 255), title);

            auto draw_header_tab = [&](const char* label, BrowserTab tab) {
                bool selected = (active_tab == tab);
                ImVec2 label_size = ImGui::CalcTextSize(label);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
                ImGui::PushStyleColor(ImGuiCol_Button,
                    selected ? ImVec4(0.12f, 0.12f, 0.14f, 1.0f)
                             : ImVec4(0.09f, 0.09f, 0.11f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.16f, 0.16f, 0.18f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.10f, 0.46f, 0.82f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_Text,
                    selected ? ImVec4(0.88f, 0.88f, 0.92f, 1.0f)
                             : ImVec4(0.58f, 0.58f, 0.64f, 1.0f));
                if (ImGui::Button(label, ImVec2(label_size.x + 24.0f, 30.0f)))
                    active_tab = tab;
                ImGui::PopStyleColor(4);
                ImGui::PopStyleVar();
            };

            float tab_x = p0.x + 20.0f + text_size.x + 24.0f;
            float tab_y = p0.y + (title_h - 30.0f) * 0.5f;
            ImGui::SetCursorScreenPos(ImVec2(tab_x, tab_y));
            draw_header_tab("Project Browser", BrowserTab::ProjectBrowser);
            ImGui::SameLine(0.0f, 4.0f);
            draw_header_tab("Open Existing Project", BrowserTab::OpenExistingProject);
            ImGui::PopFont();

            draw->AddLine(ImVec2(p0.x, p1.y), p1, IM_COL32(50, 50, 60, 255));
            ImGui::SetCursorScreenPos(p0);
            ImGui::Dummy(ImVec2(0, title_h + 1));
        }

        // ---- Content area ----
        float content_margin = io.DisplaySize.x >= 760.0f ? 44.0f : 24.0f;
        float content_w = io.DisplaySize.x - content_margin * 2.0f;
        if (content_w > 980.0f)
            content_w = 980.0f;
        if (content_w < 560.0f)
            content_w = io.DisplaySize.x - 32.0f;

        float content_x = (io.DisplaySize.x - content_w) * 0.5f;
        float content_y = ImGui::GetCursorPosY() + 30.0f;
        float content_h = io.DisplaySize.y - content_y - 34.0f;
        if (content_h < 360.0f)
            content_h = 360.0f;

        ImGui::SetCursorPos(ImVec2(content_x, content_y));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22.0f, 22.0f));
        ImGui::BeginChild("##ProjectContent", ImVec2(content_w, content_h), true);

        auto draw_title = [](const char* title, const char* subtitle, float wrap_width) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.92f, 0.94f, 0.98f, 1.0f));
            ImGui::TextUnformatted(title);
            ImGui::PopStyleColor();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.56f, 0.62f, 0.72f, 1.0f));
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap_width);
            ImGui::TextUnformatted(subtitle);
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(0.0f, 8.0f));
        };

        auto draw_label = [](const char* label) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.58f, 0.64f, 0.74f, 1.0f));
            ImGui::TextUnformatted(label);
            ImGui::PopStyleColor();
        };

        auto draw_status = [&](float wrap_width) {
            if (status_message.empty())
                return;

            ImVec4 color = status_is_error
                ? ImVec4(0.94f, 0.48f, 0.50f, 1.0f)
                : ImVec4(0.43f, 0.78f, 0.66f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, color);
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap_width);
            ImGui::TextUnformatted(status_message.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        };

        auto primary_button = [](const char* label, const ImVec2& size, bool enabled) {
            bool pressed = false;
            if (!enabled)
                ImGui::BeginDisabled();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.10f, 0.47f, 0.57f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.14f, 0.57f, 0.68f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.08f, 0.38f, 0.47f, 1.0f));
            pressed = ImGui::Button(label, size);
            ImGui::PopStyleColor(3);
            if (!enabled)
                ImGui::EndDisabled();
            return enabled && pressed;
        };

        float full_w = ImGui::GetContentRegionAvail().x;
        float browse_w = 94.0f;
        float side_w = full_w >= 740.0f ? 260.0f : 0.0f;
        float column_gap = side_w > 0.0f ? 22.0f : 0.0f;
        float form_w = full_w - side_w - column_gap;

        if (active_tab == BrowserTab::ProjectBrowser)
        {
            bool has_templates = !available_templates.empty();
            bool valid_template = has_templates &&
                                  selected_template >= 0 &&
                                  selected_template < (int)available_templates.size();
            bool can_create = valid_template &&
                              new_project_name[0] != '\0' &&
                              new_project_dir[0] != '\0';

            ImGui::BeginGroup();
            draw_title("Create Project", "Choose a template and project location. The editor opens the new .garden project after creation.", form_w);

            draw_label("Project name");
            ImGui::SetNextItemWidth(form_w);
            ImGui::InputTextWithHint("##new_name", "Project name",
                                     new_project_name, sizeof(new_project_name));

            draw_label("Template");
            if (has_templates)
            {
                const char* preview = valid_template
                    ? available_templates[selected_template].name.c_str()
                    : "Select template";
                ImGui::SetNextItemWidth(form_w);
                if (ImGui::BeginCombo("##new_template", preview))
                {
                    for (int i = 0; i < (int)available_templates.size(); i++)
                    {
                        bool is_selected = (selected_template == i);
                        if (ImGui::Selectable(available_templates[i].name.c_str(), is_selected))
                            selected_template = i;
                        if (is_selected)
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            }
            else
            {
                ImGui::TextColored(ImVec4(0.95f, 0.67f, 0.35f, 1.0f),
                    "No templates were found in the Templates directory.");
            }

            draw_label("Project directory");
            ImGui::SetNextItemWidth(form_w - browse_w - 10.0f);
            ImGui::InputTextWithHint("##new_dir", "Parent directory",
                                     new_project_dir, sizeof(new_project_dir));
            ImGui::SameLine();
            if (ImGui::Button("Browse##dir", ImVec2(browse_w, 0.0f)))
            {
                std::string folder = FileDialog::openFolder("Select Project Directory");
                if (!folder.empty())
                {
                    std::strncpy(new_project_dir, folder.c_str(),
                                 sizeof(new_project_dir) - 1);
                    new_project_dir[sizeof(new_project_dir) - 1] = '\0';
                    status_message.clear();
                }
            }

            ImGui::Dummy(ImVec2(0.0f, 10.0f));
            if (primary_button("Create Project", ImVec2(168.0f, 40.0f), can_create))
            {
                std::string name(new_project_name);
                std::string dir(new_project_dir);
                bool success = project_manager.createProjectFromTemplate(
                    available_templates[selected_template].path, dir, name);

                if (success)
                {
                    result_path = project_manager.getProjectFilePath();
                    running = false;
                }
                else
                {
                    status_is_error = true;
                    status_message = "Failed to create the project. Check the directory and template files.";
                    fprintf(stderr, "[ProjectSelector] Failed to create project '%s' in '%s'\n",
                            name.c_str(), dir.c_str());
                }
            }
            ImGui::SameLine();
            if (!can_create)
                ImGui::TextDisabled("Name, directory, and template are required.");
            else
                ImGui::TextDisabled("Ready to create.");

            ImGui::Dummy(ImVec2(0.0f, 8.0f));
            draw_status(form_w);
            ImGui::EndGroup();

            if (side_w > 0.0f)
            {
                ImGui::SameLine(0.0f, column_gap);
                ImGui::BeginChild("##TemplatePanel", ImVec2(side_w, 0.0f), true);
                ImGui::TextDisabled("Templates");
                ImGui::Separator();
                if (has_templates)
                {
                    for (int i = 0; i < (int)available_templates.size(); i++)
                    {
                        bool is_selected = (selected_template == i);
                        if (ImGui::Selectable(available_templates[i].name.c_str(), is_selected, 0, ImVec2(0.0f, 34.0f)))
                            selected_template = i;
                    }

                    if (valid_template)
                    {
                        ImGui::Separator();
                        ImGui::TextDisabled("Path");
                        ImGui::TextWrapped("%s", available_templates[selected_template].path.c_str());
                    }
                }
                else
                {
                    ImGui::TextWrapped("Install or restore a template under the engine Templates folder.");
                }
                ImGui::EndChild();
            }
        }
        else if (active_tab == BrowserTab::OpenExistingProject)
        {
            bool can_open = open_project_path[0] != '\0';

            ImGui::BeginGroup();
            draw_title("Open Project", "Select an existing .garden project file. The editor will load it directly.", form_w);

            draw_label("Project file");
            ImGui::SetNextItemWidth(form_w - browse_w - 10.0f);
            ImGui::InputTextWithHint("##open_path", "Path to .garden file",
                                     open_project_path, sizeof(open_project_path));
            ImGui::SameLine();
            if (ImGui::Button("Browse##open", ImVec2(browse_w, 0.0f)))
            {
                std::string file = FileDialog::openFile(
                    "Open Garden Project",
                    "Garden Project (*.garden)\0*.garden\0All Files (*.*)\0*.*\0");
                if (!file.empty())
                {
                    std::strncpy(open_project_path, file.c_str(),
                                 sizeof(open_project_path) - 1);
                    open_project_path[sizeof(open_project_path) - 1] = '\0';
                    status_message.clear();
                }
            }

            ImGui::Dummy(ImVec2(0.0f, 10.0f));
            if (primary_button("Open Project", ImVec2(150.0f, 40.0f), can_open))
            {
                std::filesystem::path path(open_project_path);
                std::error_code exists_error;
                bool project_exists = std::filesystem::exists(path, exists_error);
                if (exists_error || !project_exists)
                {
                    status_is_error = true;
                    status_message = "That project file does not exist.";
                }
                else
                {
                    std::error_code absolute_error;
                    std::filesystem::path absolute_path = std::filesystem::absolute(path, absolute_error);
                    if (absolute_error)
                    {
                        status_is_error = true;
                        status_message = "Could not resolve that project path.";
                    }
                    else
                    {
                        result_path = absolute_path.string();
                        running = false;
                    }
                }
            }
            ImGui::SameLine();
            if (!can_open)
                ImGui::TextDisabled("Choose a .garden file first.");
            else
                ImGui::TextDisabled("Ready to open.");

            ImGui::Dummy(ImVec2(0.0f, 8.0f));
            draw_status(form_w);
            ImGui::EndGroup();

            if (side_w > 0.0f)
            {
                ImGui::SameLine(0.0f, column_gap);
                ImGui::BeginChild("##OpenInfoPanel", ImVec2(side_w, 0.0f), true);
                ImGui::TextDisabled("Project File");
                ImGui::Separator();
                ImGui::TextWrapped("Garden project files use the .garden extension and describe the editor project root.");
                ImGui::Dummy(ImVec2(0.0f, 8.0f));
                ImGui::TextDisabled("Current path");
                ImGui::TextWrapped("%s", open_project_path[0] != '\0' ? open_project_path : "No project selected.");
                ImGui::EndChild();
            }
        }

        ImGui::EndChild(); // ##ProjectContent
        ImGui::PopStyleVar();

        ImGui::End(); // ##ProjectBrowserBG
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor();

        // Render
        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 25, 25, 30, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    // Tear down
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return result_path;
}
