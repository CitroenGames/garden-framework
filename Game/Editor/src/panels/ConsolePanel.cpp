#include "ConsolePanel.hpp"
#include "Console/Console.hpp"
#include "EditorIcons.hpp"
#include "imgui.h"
#include <cstring>
#include <algorithm>

struct ConsolePanelCallbackData
{
    ConsolePanel* panel;
    int* history_index;
};

int ConsolePanel::inputCallback(ImGuiInputTextCallbackData* data)
{
    ConsolePanelCallbackData* user = static_cast<ConsolePanelCallbackData*>(data->UserData);
    Console& console = Console::get();

    if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory)
    {
        int history_count = console.getHistoryCount();
        if (history_count == 0) return 0;

        if (data->EventKey == ImGuiKey_UpArrow)
        {
            if (*user->history_index < history_count - 1)
                (*user->history_index)++;
        }
        else if (data->EventKey == ImGuiKey_DownArrow)
        {
            if (*user->history_index > -1)
                (*user->history_index)--;
        }

        if (*user->history_index >= 0)
        {
            const std::string& hist = console.getHistoryItem(*user->history_index);
            data->DeleteChars(0, data->BufTextLen);
            data->InsertChars(0, hist.c_str());
        }
        else
        {
            data->DeleteChars(0, data->BufTextLen);
        }
    }
    else if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion)
    {
        std::string partial(data->Buf, data->CursorPos);
        auto completions = console.getCompletions(partial);
        if (completions.size() == 1)
        {
            data->DeleteChars(0, data->BufTextLen);
            data->InsertChars(0, completions[0].c_str());
            data->InsertChars(data->CursorPos, " ");
        }
    }

    return 0;
}

void ConsolePanel::draw()
{
    ImGui::Begin("Console");

    // --- Filter bar with flat toggle buttons (UE5 style) ---
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2.0f);

    auto filterToggle = [](const char* label, bool* value, const ImVec4& color)
    {
        if (*value)
            ImGui::PushStyleColor(ImGuiCol_Button, color);
        else
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.18f, 0.18f, 1.0f));

        if (ImGui::SmallButton(label))
            *value = !*value;
        ImGui::PopStyleColor();
    };

    filterToggle(ICON_FA_CIRCLE_INFO " Info", &m_show_info, ImVec4(0.3f, 0.3f, 0.6f, 1.0f));
    ImGui::SameLine();
    filterToggle(ICON_FA_TRIANGLE_EXCLAMATION " Warn", &m_show_warn, ImVec4(0.6f, 0.5f, 0.1f, 1.0f));
    ImGui::SameLine();
    filterToggle(ICON_FA_CIRCLE_EXCLAMATION " Error", &m_show_error, ImVec4(0.6f, 0.1f, 0.1f, 1.0f));

    ImGui::PopStyleVar();

    ImGui::SameLine();

    ImGui::SetNextItemWidth(150.0f);
    ImGui::InputTextWithHint("##filter", ICON_FA_SEARCH " Filter...", m_filter_buf, sizeof(m_filter_buf));
    ImGui::SameLine();

    if (ImGui::SmallButton(ICON_FA_TRASH " Clear"))
        Console::get().clear();
    ImGui::SameLine();

    ImGui::Checkbox("Auto-scroll", &m_auto_scroll);

    ImGui::Separator();

    // --- Log display with alternating row tint ---
    float footer_height = ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 1.0f));
    ImGui::BeginChild("LogRegion", ImVec2(0, -footer_height), false, ImGuiWindowFlags_HorizontalScrollbar);

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const auto& entries = Console::get().getLogEntries();
    bool has_filter = m_filter_buf[0] != '\0';
    int visible_row = 0;

    for (const auto& entry : entries)
    {
        // Level filter
        bool show = false;
        if (entry.level <= spdlog::level::info && m_show_info) show = true;
        if (entry.level == spdlog::level::warn && m_show_warn) show = true;
        if (entry.level >= spdlog::level::err && m_show_error) show = true;
        if (!show) continue;

        // Text filter
        if (has_filter)
        {
            std::string msg_lower = entry.message;
            std::string filter_lower(m_filter_buf);
            std::transform(msg_lower.begin(), msg_lower.end(), msg_lower.begin(), ::tolower);
            std::transform(filter_lower.begin(), filter_lower.end(), filter_lower.begin(), ::tolower);
            if (msg_lower.find(filter_lower) == std::string::npos)
                continue;
        }

        // Alternating row background
        if (visible_row % 2 == 1)
        {
            ImVec2 row_min = ImGui::GetCursorScreenPos();
            float row_height = ImGui::GetTextLineHeightWithSpacing();
            ImVec2 row_max(row_min.x + ImGui::GetContentRegionAvail().x, row_min.y + row_height);
            draw_list->AddRectFilled(row_min, row_max, IM_COL32(255, 255, 255, 8));
        }
        visible_row++;

        // Color by level
        ImVec4 color;
        switch (entry.level)
        {
        case spdlog::level::warn:
            color = ImVec4(1.0f, 0.9f, 0.3f, 1.0f);
            break;
        case spdlog::level::err:
        case spdlog::level::critical:
            color = ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
            break;
        case spdlog::level::debug:
        case spdlog::level::trace:
            color = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
            break;
        default:
            color = ImVec4(0.86f, 0.86f, 0.86f, 1.0f);
            break;
        }

        ImGui::PushStyleColor(ImGuiCol_Text, color);
        if (!entry.source.empty())
            ImGui::TextUnformatted(("[" + entry.source + "] " + entry.message).c_str());
        else
            ImGui::TextUnformatted(entry.message.c_str());
        ImGui::PopStyleColor();
    }

    if (m_auto_scroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        ImGui::SetScrollHereY(1.0f);

    ImGui::EndChild();
    ImGui::PopStyleVar(); // ItemSpacing

    // --- Command input with distinct background ---
    ImGui::Separator();
    ImGuiInputTextFlags input_flags =
        ImGuiInputTextFlags_EnterReturnsTrue |
        ImGuiInputTextFlags_CallbackHistory |
        ImGuiInputTextFlags_CallbackCompletion;

    ConsolePanelCallbackData cb_data;
    cb_data.panel = this;
    cb_data.history_index = &m_history_index;

    ImGui::TextDisabled(ICON_FA_CHEVRON_RIGHT);
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.08f, 0.08f, 0.08f, 1.0f));
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText("##console_input", m_input_buf, sizeof(m_input_buf), input_flags, inputCallback, &cb_data))
    {
        if (m_input_buf[0] != '\0')
        {
            Console::get().submitCommand(std::string(m_input_buf));
            m_input_buf[0] = '\0';
            m_history_index = -1;
        }
        ImGui::SetKeyboardFocusHere(-1);
    }
    ImGui::PopStyleColor();

    ImGui::End();
}
