#pragma once

class ConsolePanel
{
public:
    void draw();

private:
    char m_input_buf[512] = {0};
    char m_filter_buf[256] = {0};
    bool m_auto_scroll = true;
    bool m_show_info = true;
    bool m_show_warn = true;
    bool m_show_error = true;
    int m_history_index = -1;

    static int inputCallback(struct ImGuiInputTextCallbackData* data);
};
