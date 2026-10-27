#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <entt/entt.hpp>
#include "LevelManager.hpp"
#include "Components/IdentityComponent.hpp"

using SceneId = uint32_t;
constexpr SceneId INVALID_SCENE = 0;

enum class SceneLoadState : uint8_t
{
    Unloaded,
    Loading,
    Loaded,
    Active,
    Failed
};

enum class TransitionType : uint8_t
{
    Instant,
    FadeToBlack,
    LoadingScreen
};

struct Scene
{
    SceneId id = INVALID_SCENE;
    std::string level_path;
    LevelData level_data;
    SceneLoadState state = SceneLoadState::Unloaded;
    float load_progress = 0.0f;

    // Stable refs for the exact entities created by this scene activation.
    std::vector<EntityRef> owned_entities;

    // Key entities
    EntityRef player_entity;
    EntityRef freecam_entity;
    EntityRef player_rep_entity;
};
