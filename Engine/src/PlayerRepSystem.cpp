#include "PlayerRepSystem.hpp"
#include "PlayerRepEcsBridge.hpp"

void update_player_representations(world& game_world, bool is_freecam)
{
    PlayerRepEcsBridge::update(game_world, is_freecam);
}
