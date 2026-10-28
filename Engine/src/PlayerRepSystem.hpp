#pragma once

#include "EngineExport.h"

class world;

// Updates representations from their tracked players' presentation poses.
ENGINE_API void update_player_representations(world& game_world, bool is_freecam);
