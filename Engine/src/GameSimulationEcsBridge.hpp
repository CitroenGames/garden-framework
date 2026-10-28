#pragma once

#include <functional>

class SystemScheduler;
struct SystemDescriptor;
class world;
struct EntityRef;
namespace GameFramework { class GameModeBase; }

// EngineCore-private adapter for the legacy registry scheduler and pawn lookup.
namespace GameSimulationEcsBridge
{
void addWorldSystem(SystemScheduler& scheduler, SystemDescriptor descriptor,
                    std::function<void()> update);
EntityRef primaryPawn(world& game_world, GameFramework::GameModeBase& game_mode);
}
