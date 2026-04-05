#pragma once

struct EditorState;

class StatusBarPanel
{
public:
    bool network_pie_active = false;   // set by EditorApp each frame
    int  spawned_processes  = 0;       // count of running PIE child processes
    void draw(const EditorState& state);
};
