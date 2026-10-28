#pragma once

class world;

// EngineCore-private bridge for tracked render-pose queries and updates.
namespace PlayerRepEcsBridge
{
void update(world& game_world, bool is_freecam);
}
