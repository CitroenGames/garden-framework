// Default ConVar definitions for the engine
#include "ConVar.hpp"

// Server control
CONVAR(sv_cheats, 0, ConVarFlags::SERVER_ONLY | ConVarFlags::REPLICATED | ConVarFlags::NOTIFY,
       "Allow cheats on this server (0=disabled, 1=enabled)");

// Graphics cvars (client-side, saved to config)
CONVAR(r_fxaa, 1, ConVarFlags::ARCHIVE | ConVarFlags::CLIENT_ONLY,
       "Enable FXAA anti-aliasing");

CONVAR_BOUNDED(r_shadowquality, 2, 0, 3, ConVarFlags::ARCHIVE | ConVarFlags::CLIENT_ONLY,
               "Shadow quality (0=off, 1=low, 2=medium, 3=high)");

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
