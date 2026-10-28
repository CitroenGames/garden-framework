#pragma once

#include <cmath>

// Per-view presentation configuration. These values are transient renderer
// input; they never modify scene components or serialized content.
struct ViewmodelSettings
{
    // Zero selects the active world-view field of view / near plane.
    float fov_degrees = 0.0f;
    float near_plane = 0.0f;
    float far_plane = 1000.0f;

    // Reserved for the later depth-remap decision. Depth-clear v1 ignores it.
    float depth_max = 0.25f;
    bool clear_depth = true;
};

inline bool isViewmodelSettingsValid(const ViewmodelSettings& settings)
{
    if (!settings.clear_depth || !std::isfinite(settings.far_plane) || settings.far_plane <= 0.0f)
        return false;
    if (!std::isfinite(settings.depth_max))
        return false;
    if (settings.fov_degrees != 0.0f &&
        (!std::isfinite(settings.fov_degrees) || settings.fov_degrees <= 1.0f || settings.fov_degrees >= 179.0f))
        return false;
    if (settings.near_plane != 0.0f &&
        (!std::isfinite(settings.near_plane) || settings.near_plane <= 0.0f || settings.near_plane >= settings.far_plane))
        return false;
    return true;
}
