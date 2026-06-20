#pragma once

#include "EngineExport.h"

#include <cstdint>
#include <memory>
#include <string>
#include <entt/entt.hpp>

namespace GameFramework
{
struct ENGINE_API PlayerState
{
    uint16_t player_id = 0;
    std::string player_name = "Player";
    float score = 0.0f;
    int32_t ping_ms = 0;
    bool is_spectator = false;
    bool is_inactive = false;
    float start_time = 0.0f;
    entt::entity pawn = entt::null;
};

using PlayerStatePtr = std::shared_ptr<PlayerState>;
}
