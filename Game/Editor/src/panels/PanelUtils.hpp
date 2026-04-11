#pragma once

#include "imgui.h"
#include "imgui_internal.h"
#include <SDL3/SDL.h>

// Call immediately after ImGui::Begin(). Renders a maximize/restore button
// in the title bar (for floating windows) or as a content overlay (for docked windows).
// Also provides a right-click context menu with "Detach to Window" / "Maximize".
inline void PanelMaximizeButton()
{
    ImGuiContext& g = *GImGui;
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (!window) return;

    // --- Determine floating/docked state ---
    ImGuiViewport* vp = window->Viewport;
    bool is_floating = vp && (vp->Flags & ImGuiViewportFlags_OwnedByApp) == 0
                           && vp != ImGui::GetMainViewport();
    SDL_Window* sdl_win = is_floating ? (SDL_Window*)vp->PlatformHandle : nullptr;
    bool is_maximized = sdl_win && (SDL_GetWindowFlags(sdl_win) & SDL_WINDOW_MAXIMIZED);

    // --- Handle pending maximize (may take 2-3 frames after undocking) ---
    ImGuiID pending_id = window->GetID("##pending_max");
    if (is_floating && sdl_win && ImGui::GetStateStorage()->GetBool(pending_id, false))
    {
        SDL_MaximizeWindow(sdl_win);
        ImGui::GetStateStorage()->SetBool(pending_id, false);
        // Clear the anti-merge override so normal viewport merging resumes.
        window->WindowClass.ViewportFlagsOverrideSet &= ~ImGuiViewportFlags_NoAutoMerge;
    }
    else if (!window->DockIsActive && ImGui::GetStateStorage()->GetBool(pending_id, false))
    {
        // Window is undocked but auto-merged into the main viewport.
        // Force it onto its own viewport so it gets an SDL_Window we can maximize.
        window->WindowClass.ViewportFlagsOverrideSet |= ImGuiViewportFlags_NoAutoMerge;
    }

    // --- Title bar button for floating (non-docked) windows ---
    //
    // Proven approach from ocornut/imgui#5115: use PushClipRect to expand into
    // the title bar, then SetCursorScreenPos + InvisibleButton for interaction.
    // Regular ImGui buttons properly set g.HoveredId which prevents the window
    // drag system (UpdateMouseMovingWindowEndFrame) from stealing the click.
    //
    if (!window->DockIsActive && !(window->Flags & ImGuiWindowFlags_NoTitleBar) && window->TitleBarHeight > 0.0f)
    {
        const ImGuiStyle& style = g.Style;
        const float button_sz = g.FontSize;
        const ImRect title_bar = window->TitleBarRect();

        // Compute position: to the left of existing title bar buttons.
        // ImGui places close rightmost, then collapse (if on right side).
        float pad_r = style.FramePadding.x;
        if (window->HasCloseButton)
            pad_r += button_sz + style.ItemInnerSpacing.x;
        if (!(window->Flags & ImGuiWindowFlags_NoCollapse) && style.WindowMenuButtonPosition == ImGuiDir_Right)
            pad_r += button_sz + style.ItemInnerSpacing.x;

        const ImVec2 btn_pos(title_bar.Max.x - pad_r - button_sz,
                             title_bar.Min.y + style.FramePadding.y);

        // Save cursor state so we can restore it after
        ImVec2 saved_cursor = ImGui::GetCursorScreenPos();

        // Expand clip rect to include the title bar (Begin() restricts to content area)
        ImGui::PushClipRect(title_bar.Min, title_bar.Max, false);

        // Position cursor in the title bar and use a real InvisibleButton
        // This properly integrates with ImGui's item system (sets g.HoveredId, etc.)
        ImGui::SetCursorScreenPos(btn_pos);
        ImGui::InvisibleButton("##maximize", ImVec2(button_sz, button_sz));
        bool hovered = ImGui::IsItemHovered();
        bool held = ImGui::IsItemActive();
        bool pressed = ImGui::IsItemClicked(ImGuiMouseButton_Left);

        if (pressed)
        {
            if (is_floating && sdl_win)
            {
                if (is_maximized) SDL_RestoreWindow(sdl_win);
                else              SDL_MaximizeWindow(sdl_win);
            }
            else if (!window->DockIsActive)
            {
                // Not floating yet — trigger the pending maximize flow.
                ImGui::GetStateStorage()->SetBool(pending_id, true);
            }
        }

        // Draw button background (only on hover/active, matching ImGui style)
        ImRect bb(btn_pos, ImVec2(btn_pos.x + button_sz, btn_pos.y + button_sz));
        if (hovered || held)
        {
            ImU32 bg = ImGui::GetColorU32(held ? ImGuiCol_ButtonActive : ImGuiCol_ButtonHovered);
            window->DrawList->AddRectFilled(bb.Min, bb.Max, bg, style.FrameRounding);
        }

        // Draw maximize/restore icon
        const float icon_pad = button_sz * 0.25f;
        ImVec2 icon_min(bb.Min.x + icon_pad, bb.Min.y + icon_pad);
        ImVec2 icon_max(bb.Max.x - icon_pad, bb.Max.y - icon_pad);
        ImU32 icon_col = ImGui::GetColorU32(ImGuiCol_Text);
        if (is_maximized)
        {
            // Restore icon: two overlapping rectangles
            float off = button_sz * 0.12f;
            window->DrawList->AddRect(ImVec2(icon_min.x + off, icon_min.y),
                                      ImVec2(icon_max.x, icon_max.y - off),
                                      icon_col, 0.0f, 0, 1.0f);
            window->DrawList->AddRectFilled(ImVec2(icon_min.x, icon_min.y + off),
                                            ImVec2(icon_max.x - off, icon_max.y),
                                            ImGui::GetColorU32(ImGuiCol_TitleBgActive));
            window->DrawList->AddRect(ImVec2(icon_min.x, icon_min.y + off),
                                      ImVec2(icon_max.x - off, icon_max.y),
                                      icon_col, 0.0f, 0, 1.0f);
        }
        else
        {
            window->DrawList->AddRect(icon_min, icon_max, icon_col, 0.0f, 0, 1.0f);
        }

        // Restore clip rect and cursor position
        ImGui::PopClipRect();
        ImGui::SetCursorScreenPos(saved_cursor);
    }

    // --- For docked windows: small maximize button in top-right of content area ---
    // Uses manual hit testing + foreground draw list because child windows rendered
    // after this call overlap the button area and steal g.HoveredWindow, making
    // regular ImGui::Button unreachable.
    if (window->DockIsActive)
    {
        float btn_size = ImGui::GetFrameHeight();

        // Button rect in screen coordinates (top-right of content region)
        ImVec2 btn_min(window->ContentRegionRect.Max.x - btn_size,
                       window->ContentRegionRect.Min.y);
        ImVec2 btn_max(window->ContentRegionRect.Max.x,
                       window->ContentRegionRect.Min.y + btn_size);

        // Manual hit test — bypasses g.HoveredWindow so child windows can't block us
        ImGuiIO& io = ImGui::GetIO();
        bool hovered = ImRect(btn_min, btn_max).Contains(io.MousePos)
                    && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);

        if (hovered)
        {
            // Claim HoveredId so child window items under the button don't process clicks
            ImGuiID btn_id = window->GetID("##dock_maximize");
            ImGui::SetHoveredID(btn_id);
            ImGui::SetTooltip("Detach & Maximize");
        }

        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            ImGui::DockContextQueueUndockWindow(GImGui, window);
            ImGui::GetStateStorage()->SetBool(pending_id, true);
        }

        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            ImGui::OpenPopup("##panel_ctx");

        // Draw on foreground draw list so the icon is always visible above content
        ImDrawList* fg = ImGui::GetForegroundDrawList(window->Viewport);
        if (hovered)
        {
            fg->AddRectFilled(btn_min, btn_max,
                              ImGui::GetColorU32(ImVec4(0.3f, 0.3f, 0.3f, 0.5f)),
                              g.Style.FrameRounding);
        }

        float ip = btn_size * 0.28f;
        ImU32 icon_col = ImGui::GetColorU32(hovered ? ImGuiCol_Text : ImGuiCol_TextDisabled);
        fg->AddRect(ImVec2(btn_min.x + ip, btn_min.y + ip),
                    ImVec2(btn_max.x - ip, btn_max.y - ip),
                    icon_col, 0.0f, 0, 1.0f);
    }

    // --- Right-click context menu ---
    if (ImGui::BeginPopupContextItem("##panel_ctx"))
    {
        if (window->DockIsActive)
        {
            if (ImGui::MenuItem("Detach to Window"))
                ImGui::DockContextQueueUndockWindow(GImGui, window);
            if (ImGui::MenuItem("Detach & Maximize"))
            {
                ImGui::DockContextQueueUndockWindow(GImGui, window);
                ImGui::GetStateStorage()->SetBool(pending_id, true);
            }
        }
        if (is_floating && sdl_win)
        {
            if (ImGui::MenuItem(is_maximized ? "Restore" : "Maximize"))
            {
                if (is_maximized) SDL_RestoreWindow(sdl_win);
                else              SDL_MaximizeWindow(sdl_win);
            }
        }
        else if (!window->DockIsActive)
        {
            // Undocked but on main viewport — use pending maximize flow.
            if (ImGui::MenuItem("Maximize"))
                ImGui::GetStateStorage()->SetBool(pending_id, true);
        }
        ImGui::EndPopup();
    }
}
