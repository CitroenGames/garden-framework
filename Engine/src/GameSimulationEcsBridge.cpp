#include "GameSimulationEcsBridge.hpp"

#include "GameFramework/GameModeBase.hpp"
#include "Scheduler/SystemScheduler.hpp"
#include "world.hpp"
#include <utility>

void GameSimulationEcsBridge::addWorldSystem(SystemScheduler& scheduler,
                                             SystemDescriptor descriptor,
                                             std::function<void()> update)
{
    descriptor.update = [update = std::move(update)](entt::registry&,
                                                    const Tick::SimulationTickContext&) {
        update();
    };
    scheduler.addSystem(std::move(descriptor));
}

EntityRef GameSimulationEcsBridge::primaryPawn(world& game_world,
                                                GameFramework::GameModeBase& game_mode)
{
    const auto* player = game_mode.getPrimaryPlayer();
    return player && game_world.registry.valid(player->pawn)
        ? ensureEntityRef(game_world.registry, player->pawn) : EntityRef{};
}
