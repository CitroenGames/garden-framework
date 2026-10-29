#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "LevelManager.hpp"
#include "Components/IdentityComponent.hpp"
using SceneId = uint32_t;
constexpr SceneId INVALID_SCENE = 0;
enum class SceneLoadState : uint8_t { Unloaded, Loading, Loaded, Active, Failed };
enum class TransitionType : uint8_t { Instant, FadeToBlack, LoadingScreen };
struct Scene {
    SceneId id = INVALID_SCENE;
    std::string level_path;
    LevelData level_data;
    SceneLoadState state = SceneLoadState::Unloaded;
    float load_progress = 0.0f;
    std::vector<garden::ecs::EntityHandle> owned_entities;
    std::vector<EntityRef> owned_references;
    EntityRef player_reference{}, freecam_reference{}, player_rep_reference{};
    garden::ecs::EntityHandle player_entity{}, freecam_entity{}, player_rep_entity{};
};
