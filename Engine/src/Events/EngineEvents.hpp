#pragma once

#include <glm/glm.hpp>
#include <entt/entt.hpp>
#include <cstdint>
#include <string>
#include "Components/IdentityComponent.hpp"

// --- Physics Events ---

struct CollisionEvent
{
    entt::entity entity_a = entt::null;
    entt::entity entity_b = entt::null;
    glm::vec3 contact_point{0.0f};
    glm::vec3 contact_normal{0.0f};
    float impulse = 0.0f;
};

struct PhysicsStepEvent
{
    float delta_time = 0.0f;
};

struct TriggerEnteredEvent
{
    entt::entity trigger = entt::null;
    entt::entity other = entt::null;
    uint32_t trigger_id = 0;
};

struct TriggerExitedEvent
{
    entt::entity trigger = entt::null;
    entt::entity other = entt::null;
    uint32_t trigger_id = 0;
};


// --- Entity Lifecycle Events ---

struct EntityCreatedEvent
{
    EntityRef ref;
    std::string tag;
};

struct EntityDestroyedEvent
{
    EntityRef ref;
    std::string tag;
};

// --- Level/Scene Events ---

struct LevelLoadedEvent
{
    std::string level_name;
    std::string level_path;
};

struct LevelUnloadedEvent
{
    std::string level_name;
};

struct SceneLoadStartEvent
{
    uint32_t scene_id = 0;
    std::string level_path;
};

struct SceneLoadProgressEvent
{
    uint32_t scene_id = 0;
    float progress = 0.0f; // 0.0 to 1.0
};

struct SceneLoadCompleteEvent
{
    uint32_t scene_id = 0;
    std::string level_path;
    bool success = false;
};

struct SceneUnloadEvent
{
    uint32_t scene_id = 0;
    std::string level_path;
};

// --- Game State Events ---

struct StateChangedEvent
{
    std::string old_state;
    std::string new_state;
};

struct StatePushedEvent
{
    std::string state_name;
};

struct StatePoppedEvent
{
    std::string state_name;
};

// --- Gameplay Framework Events ---

struct GameModeInitializedEvent
{
    std::string game_mode;
    std::string map_name;
};

struct PlayerLoginEvent
{
    uint16_t player_id = 0;
    std::string player_name;
};

struct PlayerLogoutEvent
{
    uint16_t player_id = 0;
    std::string player_name;
};

struct MatchStateChangedEvent
{
    std::string old_state;
    std::string new_state;
};

// --- Timer Events ---

struct TimerExpiredEvent
{
    uint32_t timer_id = 0;
    bool repeating = false;
};

// --- Generic Gameplay Events (EngineCore, no product types) ---

struct DamageAppliedEvent
{
    EntityRef target;
    EntityRef instigator;
    EntityRef source;
    float amount = 0.0f;
    float remaining_health = 0.0f;
    uint32_t damage_type = 0;
    bool killed = false;
    uint64_t tick = 0;
};

struct EntityDiedEvent
{
    EntityRef target;
    EntityRef instigator;
    EntityRef source;
    uint32_t damage_type = 0;
    uint64_t tick = 0;
};

struct ChannelCompletedEvent
{
    EntityRef entity;
    uint32_t channel_id = 0;
    uint64_t tick = 0;
};

struct ChannelCancelledEvent
{
    EntityRef entity;
    uint32_t channel_id = 0;
    uint64_t tick = 0;
};

// --- Animation Events ---

struct AnimationFinishedEvent
{
    entt::entity entity = entt::null;
    std::string clip_name;
};

// --- Input Events ---

struct InputActionEvent
{
    std::string action_name;
    bool pressed = false;
};
