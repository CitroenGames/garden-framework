#pragma once

#include <glm/glm.hpp>
#include <entt/entt.hpp>
#include <string>

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

// --- Entity Lifecycle Events ---

struct EntityCreatedEvent
{
    entt::entity entity = entt::null;
    std::string tag;
};

struct EntityDestroyedEvent
{
    entt::entity entity = entt::null;
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

// --- Timer Events ---

struct TimerExpiredEvent
{
    uint32_t timer_id = 0;
    bool repeating = false;
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
