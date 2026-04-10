#include "ProjectSelector.hpp"
#include "Project/ProjectManager.hpp"
#include "Utils/FileDialog.hpp"
#include "Utils/EnginePaths.hpp"

#include <SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_sdlrenderer2.h>

#include <filesystem>
#include <cstring>
#include <cstdio>

std::string ProjectSelector::run()
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0)
    {
        fprintf(stderr, "[ProjectSelector] SDL_Init failed: %s\n", SDL_GetError());
        return "";
    }

    SDL_Window* window = SDL_CreateWindow(
        "Garden Engine",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        900, 600,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);

    if (!window)
    {
        fprintf(stderr, "[ProjectSelector] SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return "";
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(
        window, -1,
        SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_ACCELERATED);

    if (!renderer)
    {
        fprintf(stderr, "[ProjectSelector] SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return "";
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

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

    ProjectManager project_manager;
    std::string result_path;
    bool running = true;

    while (running)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT)
                running = false;
            if (event.type == SDL_WINDOWEVENT &&
                event.window.event == SDL_WINDOWEVENT_CLOSE)
                running = false;
        }

        if (!running)
            break;

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
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

            const char* subtitle = "Project Browser";
            ImVec2 sub_size = ImGui::CalcTextSize(subtitle);
            ImVec2 sub_pos(p0.x + 20.0f + text_size.x + 16.0f,
                           p0.y + (title_h - sub_size.y) * 0.5f);
            draw->AddText(sub_pos, IM_COL32(120, 120, 130, 255), subtitle);
            ImGui::PopFont();

            draw->AddLine(ImVec2(p0.x, p1.y), p1, IM_COL32(50, 50, 60, 255));
            ImGui::Dummy(ImVec2(0, title_h + 1));
        }

        // ---- Content area ----
        float panel_w = 560.0f;
        float panel_h = 420.0f;
        float pad_x = (io.DisplaySize.x - panel_w) * 0.5f;
        float pad_y_top = ImGui::GetCursorPosY() + 40.0f;

        ImGui::SetCursorPos(ImVec2(pad_x, pad_y_top));
        ImGui::BeginChild("##ProjectContent", ImVec2(panel_w, panel_h), false);

        // ---- Open Project section ----
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.85f, 0.88f, 1.0f));
        ImGui::TextUnformatted("Open Existing Project");
        ImGui::PopStyleColor();
        ImGui::Spacing();

        float browse_w = 80.0f;
        float label_col = 80.0f;
        float open_w = 72.0f;
        float btns_total = browse_w + 4 + open_w + 8;
        ImGui::SetNextItemWidth(panel_w - btns_total);
        ImGui::InputTextWithHint("##open_path", "Path to .garden file...",
                                 open_project_path, sizeof(open_project_path));
        ImGui::SameLine();
        if (ImGui::Button("Browse##open", ImVec2(browse_w, 0)))
        {
            std::string file = FileDialog::openFile(
                "Open Garden Project",
                "Garden Project (*.garden)\0*.garden\0All Files (*.*)\0*.*\0");
            if (!file.empty())
                std::strncpy(open_project_path, file.c_str(),
                             sizeof(open_project_path) - 1);
        }
        ImGui::SameLine();
        if (ImGui::Button("Open", ImVec2(open_w, 0)))
        {
            std::string path(open_project_path);
            if (!path.empty())
            {
                result_path = std::filesystem::absolute(path).string();
                running = false;
            }
        }

        ImGui::Spacing();
        ImGui::Spacing();

        // Divider
        {
            ImVec2 cp = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddLine(ImVec2(cp.x, cp.y), ImVec2(cp.x + panel_w, cp.y),
                        IM_COL32(50, 50, 60, 255));
            ImGui::Dummy(ImVec2(0, 1));
        }

        ImGui::Spacing();
        ImGui::Spacing();

        // ---- New Project section ----
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.85f, 0.88f, 1.0f));
        ImGui::TextUnformatted("Create New Project");
        ImGui::PopStyleColor();
        ImGui::Spacing();

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Name");
        ImGui::SameLine(label_col);
        ImGui::SetNextItemWidth(panel_w - label_col);
        ImGui::InputTextWithHint("##new_name", "Project name...",
                                 new_project_name, sizeof(new_project_name));

        // Template selection
        if (!available_templates.empty())
        {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Template");
            ImGui::SameLine(label_col);
            const char* preview = (selected_template >= 0 &&
                                   selected_template < (int)available_templates.size())
                ? available_templates[selected_template].name.c_str()
                : "Select Template...";
            ImGui::SetNextItemWidth(panel_w - label_col);
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
            ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1.0f),
                "No project templates found. Check your Templates/ directory.");
        }

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Directory");
        ImGui::SameLine(label_col);
        ImGui::SetNextItemWidth(panel_w - label_col - browse_w - 8);
        ImGui::InputTextWithHint("##new_dir", "Parent directory...",
                                 new_project_dir, sizeof(new_project_dir));
        ImGui::SameLine();
        if (ImGui::Button("Browse##dir", ImVec2(browse_w, 0)))
        {
            std::string folder = FileDialog::openFolder("Select Project Directory");
            if (!folder.empty())
                std::strncpy(new_project_dir, folder.c_str(),
                             sizeof(new_project_dir) - 1);
        }

        ImGui::Spacing();
        ImGui::Spacing();

        // Create button
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.45f, 0.80f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.50f, 0.90f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.15f, 0.40f, 0.75f, 1.0f));
        bool can_create = !available_templates.empty() &&
                          selected_template >= 0 &&
                          selected_template < (int)available_templates.size();
        if (!can_create) ImGui::BeginDisabled();
        if (ImGui::Button("Create Project", ImVec2(160, 36)))
        {
            std::string name(new_project_name);
            std::string dir(new_project_dir);
            if (!name.empty() && !dir.empty())
            {
                bool success = project_manager.createProjectFromTemplate(
                    available_templates[selected_template].path, dir, name);

                if (success)
                {
                    result_path = project_manager.getProjectFilePath();
                    running = false;
                }
                else
                {
                    fprintf(stderr, "[ProjectSelector] Failed to create project '%s' in '%s'\n",
                            name.c_str(), dir.c_str());
                }
            }
        }
        ImGui::PopStyleColor(3);
        if (!can_create) ImGui::EndDisabled();

        ImGui::EndChild(); // ##ProjectContent

        ImGui::End(); // ##ProjectBrowserBG
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor();

        // Render
        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 25, 25, 30, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    // Tear down
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return result_path;
}
