#include "EnginePicker.hpp"

#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#include <cstdio>

std::string showEnginePicker(const std::vector<EngineEntry>& engines, const std::string& projectName)
{
    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        fprintf(stderr, "SDL_Init Error: %s\n", SDL_GetError());
        return "";
    }

    SDL_SetHint(SDL_HINT_IME_IMPLEMENTED_UI, "1");

    SDL_Window* window = SDL_CreateWindow(
        "Garden - Select Engine",
        550, 400,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window)
    {
        fprintf(stderr, "SDL_CreateWindow Error: %s\n", SDL_GetError());
        SDL_Quit();
        return "";
    }
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);

    SDL_Renderer* renderer = SDL_CreateRenderer(window, NULL);
    if (!renderer)
    {
        fprintf(stderr, "SDL_CreateRenderer Error: %s\n", SDL_GetError());
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

    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    std::string selected_id;
    int selected_index = -1;
    bool done = false;

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
        }
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED)
        {
            SDL_Delay(10);
            continue;
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        // Fullscreen ImGui window
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("##EnginePicker", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

        ImGui::TextUnformatted("Select an engine for:");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "%s", projectName.c_str());
        ImGui::Separator();
        ImGui::Spacing();

        if (engines.empty())
        {
            ImGui::TextWrapped(
                "No engines are registered.\n\n"
                "Register an engine from the command line:\n"
                "  garden register-engine --path <engine_dir>");
        }
        else
        {
            // Scrollable engine list - reserve space for buttons at bottom
            float bottomHeight = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
            ImGui::BeginChild("EngineList", ImVec2(0, -bottomHeight), ImGuiChildFlags_Borders);

            for (int i = 0; i < (int)engines.size(); i++)
            {
                const auto& e = engines[i];
                bool is_selected = (selected_index == i);
                bool is_missing = !e.path_exists;

                ImGui::PushID(i);

                if (is_missing)
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 0.7f));

                // Two-line selectable: ID + version on first line, path on second
                char label[1024];
                snprintf(label, sizeof(label), "%s  (v%s)%s\n  %s",
                    e.id.c_str(),
                    e.version.c_str(),
                    is_missing ? "  [MISSING]" : "",
                    e.path.c_str());

                if (ImGui::Selectable(label, is_selected,
                    is_missing ? ImGuiSelectableFlags_Disabled : 0,
                    ImVec2(0, ImGui::GetTextLineHeight() * 2.5f)))
                {
                    if (!is_missing)
                        selected_index = i;
                }

                // Double-click to confirm immediately
                if (!is_missing && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
                {
                    selected_id = e.id;
                    done = true;
                }

                if (is_selected)
                    ImGui::SetItemDefaultFocus();

                if (is_missing)
                    ImGui::PopStyleColor();

                ImGui::PopID();
            }

            ImGui::EndChild();
        }

        // Bottom buttons
        bool can_select = selected_index >= 0 && selected_index < (int)engines.size();

        if (!can_select) ImGui::BeginDisabled();
        if (ImGui::Button("Select", ImVec2(120, 0)))
        {
            selected_id = engines[selected_index].id;
            done = true;
        }
        if (!can_select) ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0)))
            done = true;

        ImGui::End();

        // Render
        ImGui::Render();
        SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
        SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    // Cleanup
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return selected_id;
}
