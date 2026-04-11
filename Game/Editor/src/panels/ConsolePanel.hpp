#pragma once

#include <vector>
#include <string>

class ConsolePanel
{
public:
    void draw(bool* p_open = nullptr);

    // QOL
    bool shouldClearOnPlay() const { return m_clear_on_play; }

private:
    char m_input_buf[1024] = {0};
    char m_filter_buf[256] = {0};
    bool m_auto_scroll = true;
    bool m_show_info = true;
    bool m_show_warn = true;
    bool m_show_error = true;
    int m_history_index = -1;

    // Timestamps
    bool m_show_timestamps = false;

    // Duplicate collapsing
    bool m_collapse_duplicates = false;

    // Source filtering
    bool m_show_source_engine = true;
    bool m_show_source_client = true;
    bool m_show_source_lua = true;
    bool m_show_source_console = true;

    // Selection
    int m_selected_index = -1;

    // Autocomplete
    std::vector<std::string> m_autocompleteItems;
    int m_autocompleteSelectedIndex = -1;
    bool m_showAutocomplete = false;

    // Scroll tracking
    int m_new_message_count = 0;
    size_t m_last_entry_count = 0;

    // Clear on play
    bool m_clear_on_play = false;

    static int inputCallback(struct ImGuiInputTextCallbackData* data);
};
