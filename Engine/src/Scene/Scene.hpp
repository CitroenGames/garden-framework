#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <entt/entt.hpp>
#include "LevelManager.hpp"

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

    // Entities created by this scene (for cleanup)
    std::vector<entt::entity> owned_entities;

    // Key entities
    entt::entity player_entity = entt::null;
    entt::entity freecam_entity = entt::null;
    entt::entity player_rep_entity = entt::null;
};
