#pragma once

#include <cstdint>

enum class PIENetMode
{
    Standalone,      // No networking (existing GameSimulation path)
    ListenServer,    // Editor runs server + client in-process
    DedicatedServer  // Separate Server.exe process
};

enum class PIERunMode
{
    InEditor,        // All clients render as viewports inside the editor
    SeparateWindows  // All clients launched as separate Game.exe windows
};

struct NetworkPIESettings
{
    PIENetMode net_mode    = PIENetMode::Standalone;
    PIERunMode run_mode    = PIERunMode::InEditor;
    int        num_players = 1;       // 1-4
    uint16_t   server_port = 7777;
};
