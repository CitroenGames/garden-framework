#include "EnginePicker.hpp"

#define SDL_MAIN_HANDLED
#include <SDL.h>
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"

#include <cstdio>

std::string showEnginePicker(const std::vector<EngineEntry>& engines, const std::string& projectName)
{
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0)
    {
        fprintf(stderr, "SDL_Init Error: %s\n", SDL_GetError());
        return "";
    }

#ifdef SDL_HINT_IME_SHOW_UI
    SDL_SetHint(SDL_HINT_IME_SHOW_UI, "1");
#endif

    SDL_Window* window = SDL_CreateWindow(
        "Garden - Select Engine",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        550, 400,
        (SDL_WindowFlags)(SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI));
    if (!window)
    {
        fprintf(stderr, "SDL_CreateWindow Error: %s\n", SDL_GetError());
        SDL_Quit();
        return "";
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_ACCELERATED);
    if (!renderer)
    {
        fprintf(stderr, "SDL_CreateRenderer Error: %s\n", SDL_GetError());
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

    std::string selected_id;
    int selected_index = -1;
    bool done = false;

    while (!done)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT)
                done = true;
            if (event.type == SDL_WINDOWEVENT &&
                event.window.event == SDL_WINDOWEVENT_CLOSE &&
                event.window.windowID == SDL_GetWindowID(window))
                done = true;
        }
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED)
        {
            SDL_Delay(10);
            continue;
        }

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
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

                ImGui::PushID(i);

                // Two-line selectable: ID on first line, path on second
                char label[1024];
                snprintf(label, sizeof(label), "%s\n  %s", e.id.c_str(), e.path.c_str());

                if (ImGui::Selectable(label, is_selected, 0,
                    ImVec2(0, ImGui::GetTextLineHeight() * 2.5f)))
                {
                    selected_index = i;
                }

                // Double-click to confirm immediately
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
                {
                    selected_id = e.id;
                    done = true;
                }

                if (is_selected)
                    ImGui::SetItemDefaultFocus();

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
        SDL_RenderSetScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
        SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    // Cleanup
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return selected_id;
}
