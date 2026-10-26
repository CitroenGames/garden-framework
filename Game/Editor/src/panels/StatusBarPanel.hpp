#pragma once

#include <string>

struct EditorState;

class StatusBarPanel
{
public:
    bool network_pie_active = false;   // set by EditorApp each frame
    int  spawned_processes  = 0;       // count of running PIE child processes
    bool external_pie_active = false;
    std::string backend_name;
    std::string project_name;
    bool  gpu_ms_valid = false;
    float gpu_ms = 0.0f;
    void draw(const EditorState& state);
};
