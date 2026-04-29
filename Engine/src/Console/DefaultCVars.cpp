// Default ConVar definitions for the engine
#include "ConVar.hpp"
#include "Graphics/RenderAPI.hpp"

// Render backend selection (persisted, takes effect on next launch)
#ifdef __APPLE__
CONVAR(r_renderapi, "metal", ConVarFlags::ARCHIVE | ConVarFlags::CLIENT_ONLY,
       "Rendering backend (vulkan, metal). Changes take effect on next launch.");
#elif defined(_WIN32)
CONVAR(r_renderapi, "d3d12", ConVarFlags::ARCHIVE | ConVarFlags::CLIENT_ONLY,
       "Rendering backend (vulkan, d3d12). Changes take effect on next launch.");
#else
CONVAR(r_renderapi, "vulkan", ConVarFlags::ARCHIVE | ConVarFlags::CLIENT_ONLY,
       "Rendering backend (vulkan). Changes take effect on next launch.");
#endif

// Server control
CONVAR(sv_cheats, 0, ConVarFlags::SERVER_ONLY | ConVarFlags::REPLICATED | ConVarFlags::NOTIFY,
       "Allow cheats on this server (0=disabled, 1=enabled)");

// Graphics cvars (client-side, saved to config)
CONVAR(r_fxaa, 1, ConVarFlags::ARCHIVE | ConVarFlags::CLIENT_ONLY,
       "Enable FXAA anti-aliasing");

CONVAR(r_ssao, 1, ConVarFlags::ARCHIVE | ConVarFlags::CLIENT_ONLY,
       "Enable Screen Space Ambient Occlusion");

CONVAR_BOUNDED(r_shadowquality, 2, 0, 3, ConVarFlags::ARCHIVE | ConVarFlags::CLIENT_ONLY,
               "Shadow quality (0=off, 1=low, 2=medium, 3=high)");

CONVAR_BOUNDED(r_shadowcascades, 2, 1, 4, ConVarFlags::ARCHIVE | ConVarFlags::CLIENT_ONLY,
               "Vulkan shadow cascade count");

CONVAR(r_sky, 1, ConVarFlags::ARCHIVE | ConVarFlags::CLIENT_ONLY,
       "Enable skybox rendering");

CONVAR(r_lighting, 1, ConVarFlags::ARCHIVE | ConVarFlags::CLIENT_ONLY,
       "Enable lighting (0=unlit mode)");

CONVAR(r_dynamiclights, 1, ConVarFlags::ARCHIVE | ConVarFlags::CLIENT_ONLY,
       "Enable dynamic point and spot lights");

CONVAR(r_depthprepass, 1, ConVarFlags::ARCHIVE | ConVarFlags::CLIENT_ONLY,
       "Enable depth prepass for early-Z optimization");

CONVAR(r_frustumculling, 1, ConVarFlags::ARCHIVE | ConVarFlags::CLIENT_ONLY,
       "Enable BVH frustum culling");

CONVAR(r_staticmesh_chunking, 1, ConVarFlags::ARCHIVE | ConVarFlags::CLIENT_ONLY,
       "Enable spatial chunking for large static meshes");

CONVAR_BOUNDED(r_staticmesh_chunk_tris, 8192, 128, 65536, ConVarFlags::ARCHIVE | ConVarFlags::CLIENT_ONLY,
               "Target triangles per static mesh chunk");

CONVAR_BOUNDED(r_staticmesh_max_chunks, 3072, 0, 65536, ConVarFlags::ARCHIVE | ConVarFlags::CLIENT_ONLY,
               "Maximum chunks per static mesh, 0 means unlimited");

CONVAR(r_deferred, 0, ConVarFlags::ARCHIVE | ConVarFlags::CLIENT_ONLY,
       "Enable deferred rendering (opaque GBuffer + deferred lighting; transparents still forward). "
       "Supported on D3D12 and Vulkan.");

CONVAR(r_d3d12_dred, 0, ConVarFlags::ARCHIVE | ConVarFlags::CLIENT_ONLY,
       "Force-enable D3D12 DRED (Device Removed Extended Data) and GPU-Based Validation in Release builds. "
       "Captures breadcrumbs and page-fault data after TDRs; costs perf. Takes effect on next launch.");

CONVAR_BOUNDED(fps_max, 60, 0, 1000, ConVarFlags::ARCHIVE | ConVarFlags::CLIENT_ONLY,
               "Maximum frame rate (0=unlimited)");

// Example cheat cvars
CONVAR(god, 0, ConVarFlags::CHEAT | ConVarFlags::SERVER_ONLY,
       "God mode - invincibility");

CONVAR(noclip, 0, ConVarFlags::CHEAT | ConVarFlags::SERVER_ONLY,
       "Noclip mode - fly through walls");

CONVAR_BOUNDED(sv_gravity, 800.0f, 0.0f, 10000.0f, ConVarFlags::CHEAT | ConVarFlags::REPLICATED,
               "World gravity");

CONVAR_BOUNDED(sv_timescale, 1.0f, 0.1f, 10.0f, ConVarFlags::CHEAT | ConVarFlags::REPLICATED,
               "Game time scale");

// Player settings
CONVAR(name, "Player", ConVarFlags::ARCHIVE | ConVarFlags::USERINFO,
       "Player name");

CONVAR_BOUNDED(sensitivity, 2.0f, 0.1f, 10.0f, ConVarFlags::ARCHIVE | ConVarFlags::CLIENT_ONLY,
               "Mouse sensitivity");

// Network cvars
CONVAR_BOUNDED(cl_updaterate, 60, 20, 128, ConVarFlags::ARCHIVE | ConVarFlags::CLIENT_ONLY,
               "Client network update rate");

CONVAR_BOUNDED(cl_cmdrate, 60, 20, 128, ConVarFlags::ARCHIVE | ConVarFlags::CLIENT_ONLY,
               "Client command rate");

// Developer/debug cvars
CONVAR(developer, 0, ConVarFlags::ARCHIVE,
       "Developer mode - shows additional debug info");

CONVAR(con_notifytime, 4.0f, ConVarFlags::ARCHIVE | ConVarFlags::CLIENT_ONLY,
       "Console notification display time in seconds");

CONVAR(con_timestamps, 0, ConVarFlags::ARCHIVE | ConVarFlags::CLIENT_ONLY,
       "Show timestamps in console log");

CONVAR(con_collapse_duplicates, 0, ConVarFlags::ARCHIVE | ConVarFlags::CLIENT_ONLY,
       "Collapse duplicate consecutive messages in console");

CONVAR(con_clear_on_play, 0, ConVarFlags::ARCHIVE | ConVarFlags::CLIENT_ONLY,
       "Clear console when entering play mode");

// Editor packaging
CONVAR(editor_package_output_dir, "", ConVarFlags::ARCHIVE | ConVarFlags::CLIENT_ONLY,
       "Last used packaging output directory");

// Window settings
CONVAR_BOUNDED(window_width, 1600, 320, 7680, ConVarFlags::ARCHIVE | ConVarFlags::CLIENT_ONLY,
               "Window width");

CONVAR_BOUNDED(window_height, 900, 240, 4320, ConVarFlags::ARCHIVE | ConVarFlags::CLIENT_ONLY,
               "Window height");

CONVAR(window_maximized, 0, ConVarFlags::ARCHIVE | ConVarFlags::CLIENT_ONLY,
       "Window maximized state");

// This function forces the linker to include this translation unit.
// Without it, static library optimization strips all the static cvar objects.
void InitializeDefaultCVars()
{
    // Intentionally empty - cvars register via static initialization above
}
