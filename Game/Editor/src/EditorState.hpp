#pragma once

#include <string>
#include <cstddef>
#include "NetworkPIESettings.hpp"

enum class PlayMode
{
    Editing,  // Normal editor mode, no simulation
    Playing,  // Simulation running, game camera active, mouse captured
    Paused,   // Simulation frozen, game camera active, mouse released
    Ejected   // Simulation running, editor camera active, mouse released
};

struct EditorState
{
    enum class TransformMode { Translate, Rotate, Scale };
    TransformMode transform_mode = TransformMode::Translate;

    enum class GizmoSpace { Local, World };
    GizmoSpace gizmo_space = GizmoSpace::World;
    bool gizmo_using = false; // true while ImGuizmo drag is active

    // Snapping
    bool snap_enabled = false;
    float snap_translate = 0.5f;
    float snap_rotate = 15.0f;
    float snap_scale = 0.1f;

    // Grid
    bool show_grid = true;

    // Play mode
    PlayMode play_mode = PlayMode::Editing;

    bool isSimulationActive() const
    {
        return play_mode == PlayMode::Playing
            || play_mode == PlayMode::Paused
            || play_mode == PlayMode::Ejected;
    }

    bool isSimulationRunning() const
    {
        return play_mode == PlayMode::Playing
            || play_mode == PlayMode::Ejected;
    }

    // Render stats (updated per frame by EditorApp)
    float fps = 0.0f;
    float delta_time = 0.0f;
    size_t total_entities = 0;
    size_t visible_entities = 0;
    size_t draw_calls = 0;

    // Level info
    std::string current_save_path;
    bool unsaved_changes = false;

    // Network PIE
    NetworkPIESettings network_pie;
};
