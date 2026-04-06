#include "ConsolePanel.hpp"
#include "Console/Console.hpp"
#include "EditorIcons.hpp"
#include "imgui.h"
#include <cstring>
#include <algorithm>
#include <ctime>

struct ConsolePanelCallbackData
{
    ConsolePanel* panel;
    int* history_index;
    std::vector<std::string>* autocompleteItems;
    int* autocompleteSelectedIndex;
    bool* showAutocomplete;
};

int ConsolePanel::inputCallback(ImGuiInputTextCallbackData* data)
{
    ConsolePanelCallbackData* user = static_cast<ConsolePanelCallbackData*>(data->UserData);
    Console& console = Console::get();

    if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory)
    {
        // Don't navigate history when autocomplete is open (arrows used for popup)
        if (*user->showAutocomplete && !user->autocompleteItems->empty())
            return 0;

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
        // Tab completion
        if (*user->showAutocomplete && !user->autocompleteItems->empty())
        {
            int idx = *user->autocompleteSelectedIndex >= 0 ? *user->autocompleteSelectedIndex : 0;
            if (idx < static_cast<int>(user->autocompleteItems->size()))
            {
                data->DeleteChars(0, data->BufTextLen);
                data->InsertChars(0, (*user->autocompleteItems)[idx].c_str());
                data->InsertChars(data->CursorPos, " ");
                *user->showAutocomplete = false;
                user->autocompleteItems->clear();
                *user->autocompleteSelectedIndex = -1;
            }
        }
        else
        {
            std::string partial(data->Buf, data->CursorPos);
            auto completions = console.getCompletions(partial);

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
                        j++;
                    prefix = prefix.substr(0, j);
                }
                if (prefix.size() > partial.size())
                {
                    data->DeleteChars(0, data->BufTextLen);
                    data->InsertChars(0, prefix.c_str());
                }
                // Show popup with all matches
                *user->autocompleteItems = std::move(completions);
                *user->showAutocomplete = true;
                *user->autocompleteSelectedIndex = 0;
            }
        }
    }
    else if (data->EventFlag == ImGuiInputTextFlags_CallbackEdit)
    {
        // Update autocomplete as user types
        std::string partial(data->Buf, data->BufTextLen);
        if (partial.length() >= 2)
        {
            *user->autocompleteItems = console.getCompletions(partial);
            *user->showAutocomplete = !user->autocompleteItems->empty();
            *user->autocompleteSelectedIndex = *user->showAutocomplete ? 0 : -1;
        }
        else
        {
            *user->showAutocomplete = false;
            user->autocompleteItems->clear();
            *user->autocompleteSelectedIndex = -1;
        }
    }

    return 0;
}

// Helper: format timestamp from ms since epoch
static void FormatTimestamp(uint64_t timestamp_ms, char* buf, size_t buf_size)
{
    time_t seconds = static_cast<time_t>(timestamp_ms / 1000);
    struct tm local_tm;
#ifdef _WIN32
    localtime_s(&local_tm, &seconds);
#else
    localtime_r(&seconds, &local_tm);
#endif
    strftime(buf, buf_size, "%H:%M:%S", &local_tm);
}

// Helper: check if source passes filter
static bool PassesSourceFilter(const std::string& source,
                               bool show_engine, bool show_client,
                               bool show_lua, bool show_console)
{
    if (source == "Engine") return show_engine;
    if (source == "Client") return show_client;
    if (source == "LUA") return show_lua;
    if (source == "Console") return show_console;
    return true; // unknown sources always shown
}

void ConsolePanel::draw()
{
    ImGui::Begin("Console");

    Console& console = Console::get();

    // --- Toolbar row 1: Level filters with count badges ---
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

    char info_label[64], warn_label[64], error_label[64];
    snprintf(info_label, sizeof(info_label), ICON_FA_CIRCLE_INFO " Info (%d)", console.getInfoCount());
    snprintf(warn_label, sizeof(warn_label), ICON_FA_TRIANGLE_EXCLAMATION " Warn (%d)", console.getWarnCount());
    snprintf(error_label, sizeof(error_label), ICON_FA_CIRCLE_EXCLAMATION " Error (%d)", console.getErrorCount());

    filterToggle(info_label, &m_show_info, ImVec4(0.3f, 0.3f, 0.6f, 1.0f));
    ImGui::SameLine();
    filterToggle(warn_label, &m_show_warn, ImVec4(0.6f, 0.5f, 0.1f, 1.0f));
    ImGui::SameLine();
    filterToggle(error_label, &m_show_error, ImVec4(0.6f, 0.1f, 0.1f, 1.0f));

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();

    // Timestamp toggle
    filterToggle(ICON_FA_CLOCK "##timestamps", &m_show_timestamps, ImVec4(0.4f, 0.4f, 0.4f, 1.0f));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle timestamps");
    ImGui::SameLine();

    // Collapse duplicates toggle
    filterToggle(ICON_FA_LAYER_GROUP "##collapse", &m_collapse_duplicates, ImVec4(0.4f, 0.4f, 0.4f, 1.0f));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Collapse duplicate messages");

    ImGui::PopStyleVar();

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();

    // Source filters
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2.0f);
    filterToggle("Eng##src", &m_show_source_engine, ImVec4(0.3f, 0.4f, 0.5f, 1.0f));
    ImGui::SameLine();
    filterToggle("Cli##src", &m_show_source_client, ImVec4(0.3f, 0.5f, 0.3f, 1.0f));
    ImGui::SameLine();
    filterToggle("LUA##src", &m_show_source_lua, ImVec4(0.5f, 0.3f, 0.5f, 1.0f));
    ImGui::SameLine();
    filterToggle("Con##src", &m_show_source_console, ImVec4(0.4f, 0.4f, 0.4f, 1.0f));
    ImGui::PopStyleVar();

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();

    // Text filter
    ImGui::SetNextItemWidth(150.0f);
    ImGui::InputTextWithHint("##filter", ICON_FA_SEARCH " Filter...", m_filter_buf, sizeof(m_filter_buf));
    ImGui::SameLine();

    if (ImGui::SmallButton(ICON_FA_TRASH " Clear"))
        console.clear();
    ImGui::SameLine();

    ImGui::Checkbox("Auto-scroll", &m_auto_scroll);
    ImGui::SameLine();
    ImGui::Checkbox("Clear on Play", &m_clear_on_play);

    ImGui::Separator();

    // --- Log display ---
    float footer_height = ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 1.0f));
    ImGui::BeginChild("LogRegion", ImVec2(0, -footer_height), false, ImGuiWindowFlags_HorizontalScrollbar);

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const auto& entries = console.getLogEntries();
    bool has_filter = m_filter_buf[0] != '\0';

    // Track new messages
    size_t current_count = entries.size();
    if (current_count > m_last_entry_count)
        m_new_message_count += static_cast<int>(current_count - m_last_entry_count);
    m_last_entry_count = current_count;

    int visible_row = 0;

    // For duplicate collapsing
    const ConsoleLogEntry* pending_entry = nullptr;
    int pending_count = 0;
    std::string pending_display;

    auto renderEntry = [&](const ConsoleLogEntry& entry, int dup_count)
    {
        // Alternating row background
        if (visible_row % 2 == 1)
        {
            ImVec2 row_min = ImGui::GetCursorScreenPos();
            float row_height = ImGui::GetTextLineHeightWithSpacing();
            ImVec2 row_max(row_min.x + ImGui::GetContentRegionAvail().x, row_min.y + row_height);
            draw_list->AddRectFilled(row_min, row_max, IM_COL32(255, 255, 255, 8));
        }

        // Color by type/level
        ImVec4 color;
        if (entry.type == ConsoleEntryType::CommandEcho)
        {
            color = ImVec4(0.5f, 0.7f, 0.9f, 1.0f); // Cyan for command echoes
        }
        else
        {
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
        }

        // Build display string
        std::string display;

        // Timestamp
        if (m_show_timestamps)
        {
            char time_buf[16];
            FormatTimestamp(entry.timestamp, time_buf, sizeof(time_buf));
            display += "[";
            display += time_buf;
            display += "] ";
        }

        // Prefix
        if (entry.type == ConsoleEntryType::CommandEcho)
        {
            display += "> ";
        }
        else if (!entry.source.empty())
        {
            display += "[";
            display += entry.source;
            display += "] ";
        }

        display += entry.message;

        // Duplicate count
        if (dup_count > 1)
        {
            display += " (x";
            display += std::to_string(dup_count);
            display += ")";
        }

        ImGui::PushStyleColor(ImGuiCol_Text, color);

        // Use Selectable for click-to-select and context menu
        char selectable_id[32];
        snprintf(selectable_id, sizeof(selectable_id), "##log_%d", visible_row);
        std::string full_label = display + selectable_id;

        bool selected = (m_selected_index == visible_row);
        if (ImGui::Selectable(full_label.c_str(), selected, ImGuiSelectableFlags_AllowOverlap))
        {
            m_selected_index = visible_row;
        }

        // Right-click context menu
        if (ImGui::BeginPopupContextItem())
        {
            if (ImGui::MenuItem(ICON_FA_COPY " Copy Message"))
            {
                ImGui::SetClipboardText(entry.message.c_str());
            }
            if (ImGui::MenuItem(ICON_FA_COPY " Copy Line"))
            {
                ImGui::SetClipboardText(display.c_str());
            }
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_COPY " Copy All Visible"))
            {
                // Build text of all visible entries
                std::string all_text;
                for (const auto& e : entries)
                {
                    // Apply same filters
                    bool show = false;
                    if (e.level <= spdlog::level::info && m_show_info) show = true;
                    if (e.level == spdlog::level::warn && m_show_warn) show = true;
                    if (e.level >= spdlog::level::err && m_show_error) show = true;
                    if (!show) continue;
                    if (!PassesSourceFilter(e.source, m_show_source_engine, m_show_source_client, m_show_source_lua, m_show_source_console)) continue;

                    if (!e.source.empty())
                        all_text += "[" + e.source + "] ";
                    all_text += e.message + "\n";
                }
                ImGui::SetClipboardText(all_text.c_str());
            }
            ImGui::EndPopup();
        }

        ImGui::PopStyleColor();
        visible_row++;
    };

    for (const auto& entry : entries)
    {
        // Level filter
        bool show = false;
        if (entry.level <= spdlog::level::info && m_show_info) show = true;
        if (entry.level == spdlog::level::warn && m_show_warn) show = true;
        if (entry.level >= spdlog::level::err && m_show_error) show = true;
        if (!show) continue;

        // Source filter
        if (!PassesSourceFilter(entry.source, m_show_source_engine, m_show_source_client, m_show_source_lua, m_show_source_console))
            continue;

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

        // Duplicate collapsing
        if (m_collapse_duplicates)
        {
            if (pending_entry && pending_entry->message == entry.message &&
                pending_entry->level == entry.level && pending_entry->source == entry.source)
            {
                pending_count++;
                continue;
            }

            // Flush previous pending entry
            if (pending_entry)
            {
                renderEntry(*pending_entry, pending_count);
            }

            pending_entry = &entry;
            pending_count = 1;
        }
        else
        {
            renderEntry(entry, 1);
        }
    }

    // Flush last pending entry
    if (m_collapse_duplicates && pending_entry)
    {
        renderEntry(*pending_entry, pending_count);
    }

    // Check scroll position before ending child
    float scroll_y = ImGui::GetScrollY();
    float scroll_max = ImGui::GetScrollMaxY();
    bool at_bottom = scroll_y >= scroll_max - 1.0f;

    if (at_bottom)
        m_new_message_count = 0;

    if (m_auto_scroll && at_bottom)
        ImGui::SetScrollHereY(1.0f);

    // Scroll-to-bottom floating button (rendered inside child window, bottom-right)
    if (!at_bottom && m_new_message_count > 0)
    {
        char btn_label[64];
        snprintf(btn_label, sizeof(btn_label), ICON_FA_ANGLES_DOWN " %d new ##scrollbtn", m_new_message_count);

        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImVec2 window_pos = ImGui::GetWindowPos();
        ImVec2 window_size = ImGui::GetWindowSize();
        float btn_width = 100.0f;
        float btn_height = ImGui::GetFrameHeight();

        ImGui::SetCursorScreenPos(ImVec2(
            window_pos.x + window_size.x - btn_width - 20.0f,
            window_pos.y + window_size.y - btn_height - 8.0f
        ));

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.35f, 0.55f, 0.9f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.45f, 0.65f, 0.95f));
        if (ImGui::SmallButton(btn_label))
        {
            ImGui::SetScrollHereY(1.0f);
            m_new_message_count = 0;
            m_auto_scroll = true;
        }
        ImGui::PopStyleColor(2);
    }

    ImGui::EndChild();
    ImGui::PopStyleVar(); // ItemSpacing

    // --- Command input with autocomplete ---
    ImGui::Separator();
    ImGuiInputTextFlags input_flags =
        ImGuiInputTextFlags_EnterReturnsTrue |
        ImGuiInputTextFlags_CallbackHistory |
        ImGuiInputTextFlags_CallbackCompletion |
        ImGuiInputTextFlags_CallbackEdit;

    ConsolePanelCallbackData cb_data;
    cb_data.panel = this;
    cb_data.history_index = &m_history_index;
    cb_data.autocompleteItems = &m_autocompleteItems;
    cb_data.autocompleteSelectedIndex = &m_autocompleteSelectedIndex;
    cb_data.showAutocomplete = &m_showAutocomplete;

    ImGui::TextDisabled(ICON_FA_CHEVRON_RIGHT);
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.08f, 0.08f, 0.08f, 1.0f));
    ImGui::SetNextItemWidth(-1.0f);

    bool input_submitted = ImGui::InputText("##console_input", m_input_buf, sizeof(m_input_buf), input_flags, inputCallback, &cb_data);

    // Get input field position for autocomplete popup
    ImVec2 input_pos = ImGui::GetItemRectMin();
    float input_width = ImGui::GetItemRectSize().x;

    ImGui::PopStyleColor();

    // Handle autocomplete arrow key navigation
    if (m_showAutocomplete && !m_autocompleteItems.empty())
    {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            m_showAutocomplete = false;
            m_autocompleteItems.clear();
            m_autocompleteSelectedIndex = -1;
        }
    }

    if (input_submitted)
    {
        if (m_input_buf[0] != '\0')
        {
            console.submitCommand(std::string(m_input_buf));
            m_input_buf[0] = '\0';
            m_history_index = -1;
            m_auto_scroll = true;
        }
        m_showAutocomplete = false;
        m_autocompleteItems.clear();
        m_autocompleteSelectedIndex = -1;
        ImGui::SetKeyboardFocusHere(-1);
    }

    // Autocomplete popup (rendered above the input field)
    if (m_showAutocomplete && !m_autocompleteItems.empty())
    {
        int display_count = std::min(static_cast<int>(m_autocompleteItems.size()), 10);
        float popup_height = display_count * ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().WindowPadding.y * 2;
        if (m_autocompleteItems.size() > 10)
            popup_height += ImGui::GetTextLineHeightWithSpacing(); // "... and N more"

        // Position above input field
        ImGui::SetNextWindowPos(ImVec2(input_pos.x, input_pos.y - popup_height));
        ImGui::SetNextWindowSize(ImVec2(input_width, 0));

        ImGuiWindowFlags popup_flags = ImGuiWindowFlags_NoTitleBar |
                                       ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoFocusOnAppearing;

        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.15f, 0.15f, 0.15f, 0.96f));
        ImGui::Begin("##EditorAutoComplete", nullptr, popup_flags);

        for (int i = 0; i < display_count; i++)
        {
            bool is_selected = (i == m_autocompleteSelectedIndex);
            if (ImGui::Selectable(m_autocompleteItems[i].c_str(), is_selected))
            {
                std::strncpy(m_input_buf, m_autocompleteItems[i].c_str(), sizeof(m_input_buf) - 2);
                m_input_buf[sizeof(m_input_buf) - 2] = '\0';
                std::strncat(m_input_buf, " ", sizeof(m_input_buf) - strlen(m_input_buf) - 1);
                m_showAutocomplete = false;
                m_autocompleteItems.clear();
                m_autocompleteSelectedIndex = -1;
                ImGui::SetKeyboardFocusHere(-1);
            }
        }

        if (m_autocompleteItems.size() > 10)
        {
            ImGui::TextDisabled("... and %d more", static_cast<int>(m_autocompleteItems.size()) - 10);
        }

        ImGui::End();
        ImGui::PopStyleColor();
    }

    ImGui::End();
}
