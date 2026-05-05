#include "Character/CharacterControllerSystem.hpp"

#include "Components/Components.hpp"
#include "Utils/Log.hpp"

#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/ShapeFilter.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <algorithm>
#include <cmath>
#include <glm/common.hpp>

namespace
{
    constexpr float kMaxCharacterVelocity = 100.0f;

    uint64_t entityUserData(entt::entity entity)
    {
        return static_cast<uint64_t>(entt::to_integral(entity));
    }

    JPH::Vec3 toJolt(const glm::vec3& v)
    {
        return JPH::Vec3(v.x, v.y, v.z);
    }

    JPH::RVec3 toJoltR(const glm::vec3& v)
    {
        return JPH::RVec3(v.x, v.y, v.z);
    }

    glm::vec3 toGlm(const JPH::Vec3& v)
    {
        return glm::vec3(v.GetX(), v.GetY(), v.GetZ());
    }

    glm::vec3 toGlmR(const JPH::RVec3& v)
    {
        return glm::vec3(float(v.GetX()), float(v.GetY()), float(v.GetZ()));
    }

    bool isValidVec3(const glm::vec3& v)
    {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    }

    glm::vec3 clampVelocity(const glm::vec3& v)
    {
        const float len = glm::length(v);
        if (len > kMaxCharacterVelocity)
            return v * (kMaxCharacterVelocity / len);
        return v;
    }

    bool isCharacterGrounded(JPH::CharacterBase::EGroundState state)
    {
        return state == JPH::CharacterBase::EGroundState::OnGround;
    }

    void syncCharacterStateToComponents(entt::registry& registry,
                                        entt::entity entity,
                                        const CharacterControllerState& state)
    {
        if (!registry.valid(entity))
            return;

        if (registry.all_of<TransformComponent>(entity))
            registry.get<TransformComponent>(entity).position = state.position;

        if (registry.all_of<RigidBodyComponent>(entity))
            registry.get<RigidBodyComponent>(entity).velocity = state.velocity;

        if (registry.all_of<CharacterControllerComponent>(entity))
        {
            auto& cc = registry.get<CharacterControllerComponent>(entity);
            cc.grounded = state.grounded;
            cc.ground_normal = state.ground_normal;
            cc.ground_velocity = state.ground_velocity;
        }

        if (registry.all_of<PlayerComponent>(entity))
        {
            auto& player = registry.get<PlayerComponent>(entity);
            player.grounded = state.grounded;
            player.ground_normal = state.ground_normal;
        }
    }
}

struct CharacterControllerSystem::Runtime
{
    JPH::Ref<JPH::CharacterVirtual> character;
    JPH::ShapeRefC shape;
};

CharacterControllerSystem::CharacterControllerSystem()
    : character_vs_character_collision(std::make_unique<JPH::CharacterVsCharacterCollisionSimple>())
{
}

CharacterControllerSystem::~CharacterControllerSystem() = default;
CharacterControllerSystem::CharacterControllerSystem(CharacterControllerSystem&&) noexcept = default;
CharacterControllerSystem& CharacterControllerSystem::operator=(CharacterControllerSystem&&) noexcept = default;

void CharacterControllerSystem::shutdown(BodyEntityMap& body_to_entity)
{
    for (auto& [entity, runtime] : entity_to_character)
    {
        if (runtime && runtime->character)
        {
            character_vs_character_collision->Remove(runtime->character);
            JPH::BodyID inner_body = runtime->character->GetInnerBodyID();
            if (!inner_body.IsInvalid())
                body_to_entity.erase(inner_body);
        }
    }

    entity_to_character.clear();
}

void CharacterControllerSystem::copyPlayerSettings(entt::registry& registry, entt::entity entity)
{
    if (!registry.valid(entity) || !registry.all_of<PlayerComponent>(entity))
        return;

    const auto& player = registry.get<PlayerComponent>(entity);
    auto& controller = registry.get_or_emplace<CharacterControllerComponent>(entity);
    controller.move_speed = player.speed;
    controller.jump_velocity = player.jump_force;
    controller.capsule_half_height = player.capsule_half_height;
    controller.capsule_radius = player.capsule_radius;
    controller.input_enabled = player.input_enabled;

    if (registry.all_of<RigidBodyComponent>(entity))
        controller.mass = registry.get<RigidBodyComponent>(entity).mass;
}

JPH::BodyID CharacterControllerSystem::create(entt::registry& registry,
                                              entt::entity entity,
                                              JPH::PhysicsSystem& physics_system,
                                              JPH::TempAllocator& temp_allocator,
                                              BodyEntityMap& body_to_entity,
                                              JPH::ObjectLayer moving_layer)
{
    if (!registry.valid(entity))
        return JPH::BodyID();
    if (!registry.all_of<TransformComponent, CharacterControllerComponent>(entity))
        return JPH::BodyID();

    remove(entity, body_to_entity);

    const auto& transform = registry.get<TransformComponent>(entity);
    auto& controller = registry.get<CharacterControllerComponent>(entity);

    controller.capsule_half_height = std::max(controller.capsule_half_height, 0.01f);
    controller.capsule_radius = std::max(controller.capsule_radius, 0.01f);
    controller.character_padding = std::max(controller.character_padding, 0.0f);
    controller.collision_tolerance = std::max(controller.collision_tolerance, 0.0f);

    JPH::CapsuleShapeSettings capsule_settings(controller.capsule_half_height, controller.capsule_radius);
    JPH::ShapeSettings::ShapeResult shape_result = capsule_settings.Create();
    if (!shape_result.IsValid())
    {
        LOG_ENGINE_ERROR("Failed to create character capsule shape: {}", shape_result.GetError().c_str());
        return JPH::BodyID();
    }

    JPH::ShapeRefC shape = shape_result.Get();
    JPH::Ref<JPH::CharacterVirtualSettings> settings = new JPH::CharacterVirtualSettings();
    settings->mShape = shape;
    settings->mMass = std::max(controller.mass, 0.001f);
    settings->mMaxStrength = std::max(controller.max_strength, 0.0f);
    settings->mUp = JPH::Vec3::sAxisY();
    settings->mMaxSlopeAngle = JPH::DegreesToRadians(glm::clamp(controller.max_slope_angle, 0.0f, 89.0f));
    settings->mCharacterPadding = controller.character_padding;
    settings->mCollisionTolerance = controller.collision_tolerance;
    settings->mEnhancedInternalEdgeRemoval = controller.enhanced_internal_edge_removal;
    settings->mInnerBodyLayer = moving_layer;
    if (controller.use_inner_body)
        settings->mInnerBodyShape = shape;

    JPH::AABox bounds = shape->GetLocalBounds();
    float support_plane_y = bounds.mMin.GetY() + bounds.GetSize().GetY() * 0.25f;
    settings->mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -support_plane_y);

    auto runtime = std::make_unique<Runtime>();
    runtime->shape = shape;
    runtime->character = new JPH::CharacterVirtual(settings, toJoltR(transform.position),
        JPH::Quat::sIdentity(), entityUserData(entity), &physics_system);
    runtime->character->SetCharacterVsCharacterCollision(character_vs_character_collision.get());
    character_vs_character_collision->Add(runtime->character);

    JPH::BodyID inner_body = runtime->character->GetInnerBodyID();
    if (!inner_body.IsInvalid())
        body_to_entity[inner_body] = entity;

    entity_to_character[entity] = std::move(runtime);
    refresh(registry, entity, physics_system, temp_allocator, moving_layer);

    LOG_ENGINE_INFO("Created character controller at ({}, {}, {})",
        transform.position.x, transform.position.y, transform.position.z);
    return inner_body;
}

void CharacterControllerSystem::remove(entt::entity entity, BodyEntityMap& body_to_entity)
{
    auto it = entity_to_character.find(entity);
    if (it == entity_to_character.end())
        return;

    if (it->second && it->second->character)
    {
        character_vs_character_collision->Remove(it->second->character);
        JPH::BodyID inner_body = it->second->character->GetInnerBodyID();
        if (!inner_body.IsInvalid())
            body_to_entity.erase(inner_body);
    }

    entity_to_character.erase(it);
}

bool CharacterControllerSystem::has(entt::entity entity) const
{
    return entity_to_character.find(entity) != entity_to_character.end();
}

CharacterControllerState CharacterControllerSystem::getState(entt::registry& registry, entt::entity entity) const
{
    CharacterControllerState state;
    if (!registry.valid(entity))
        return state;

    if (registry.all_of<TransformComponent>(entity))
        state.position = registry.get<TransformComponent>(entity).position;
    if (registry.all_of<RigidBodyComponent>(entity))
        state.velocity = registry.get<RigidBodyComponent>(entity).velocity;
    if (registry.all_of<CharacterControllerComponent>(entity))
    {
        const auto& controller = registry.get<CharacterControllerComponent>(entity);
        state.grounded = controller.grounded;
        state.ground_normal = controller.ground_normal;
        state.ground_velocity = controller.ground_velocity;
    }
    else if (registry.all_of<PlayerComponent>(entity))
    {
        const auto& player = registry.get<PlayerComponent>(entity);
        state.grounded = player.grounded;
        state.ground_normal = player.ground_normal;
    }

    auto it = entity_to_character.find(entity);
    if (it != entity_to_character.end() && it->second && it->second->character)
    {
        const JPH::CharacterVirtual& character = *it->second->character;
        state.position = toGlmR(character.GetPosition());
        state.velocity = toGlm(character.GetLinearVelocity());
        state.grounded = isCharacterGrounded(character.GetGroundState());
        state.ground_normal = character.IsSupported() ? toGlm(character.GetGroundNormal()) : glm::vec3(0, 1, 0);
        state.ground_velocity = toGlm(character.GetGroundVelocity());
    }

    return state;
}

bool CharacterControllerSystem::setState(entt::registry& registry,
                                         entt::entity entity,
                                         const CharacterControllerState& state,
                                         JPH::PhysicsSystem& physics_system,
                                         JPH::TempAllocator& temp_allocator,
                                         JPH::ObjectLayer moving_layer)
{
    if (!registry.valid(entity))
        return false;

    if (registry.all_of<TransformComponent>(entity))
        registry.get<TransformComponent>(entity).position = state.position;
    if (registry.all_of<RigidBodyComponent>(entity))
        registry.get<RigidBodyComponent>(entity).velocity = state.velocity;

    syncCharacterStateToComponents(registry, entity, state);

    auto it = entity_to_character.find(entity);
    if (it == entity_to_character.end() || !it->second || !it->second->character)
        return false;

    it->second->character->SetPosition(toJoltR(state.position));
    it->second->character->SetLinearVelocity(toJolt(state.velocity));
    refresh(registry, entity, physics_system, temp_allocator, moving_layer);
    return true;
}

bool CharacterControllerSystem::teleport(entt::registry& registry,
                                         entt::entity entity,
                                         const glm::vec3& position,
                                         JPH::PhysicsSystem& physics_system,
                                         JPH::TempAllocator& temp_allocator,
                                         JPH::ObjectLayer moving_layer)
{
    if (!registry.valid(entity))
        return false;

    CharacterControllerState state = getState(registry, entity);
    state.position = position;
    state.velocity = glm::vec3(0.0f);
    return setState(registry, entity, state, physics_system, temp_allocator, moving_layer);
}

bool CharacterControllerSystem::refresh(entt::registry& registry,
                                        entt::entity entity,
                                        JPH::PhysicsSystem& physics_system,
                                        JPH::TempAllocator& temp_allocator,
                                        JPH::ObjectLayer moving_layer)
{
    auto it = entity_to_character.find(entity);
    if (it == entity_to_character.end() || !it->second || !it->second->character)
        return false;
    if (!registry.valid(entity))
        return false;

    auto& character = *it->second->character;
    if (registry.all_of<TransformComponent>(entity))
    {
        const glm::vec3 ecs_position = registry.get<TransformComponent>(entity).position;
        if (glm::distance(ecs_position, toGlmR(character.GetPosition())) > 0.001f)
            character.SetPosition(toJoltR(ecs_position));
    }

    JPH::BodyFilter body_filter;
    JPH::ShapeFilter shape_filter;
    character.RefreshContacts(
        physics_system.GetDefaultBroadPhaseLayerFilter(moving_layer),
        physics_system.GetDefaultLayerFilter(moving_layer),
        body_filter,
        shape_filter,
        temp_allocator);
    character.UpdateGroundVelocity();

    CharacterControllerState state = getState(registry, entity);
    syncCharacterStateToComponents(registry, entity, state);
    return true;
}

CharacterControllerState CharacterControllerSystem::simulate(entt::registry& registry,
                                                             entt::entity entity,
                                                             const CharacterMoveInput& input,
                                                             float delta_time,
                                                             const glm::vec3& gravity,
                                                             float fixed_delta,
                                                             JPH::PhysicsSystem& physics_system,
                                                             JPH::TempAllocator& temp_allocator,
                                                             BodyEntityMap& body_to_entity,
                                                             JPH::ObjectLayer moving_layer)
{
    if (!registry.valid(entity))
        return {};
    if (!registry.all_of<TransformComponent, CharacterControllerComponent>(entity))
        return {};

    if (entity_to_character.find(entity) == entity_to_character.end())
        create(registry, entity, physics_system, temp_allocator, body_to_entity, moving_layer);

    auto it = entity_to_character.find(entity);
    if (it == entity_to_character.end() || !it->second || !it->second->character)
        return getState(registry, entity);

    if (delta_time <= 0.0f)
        delta_time = fixed_delta;

    auto& controller = registry.get<CharacterControllerComponent>(entity);
    auto& character = *it->second->character;

    refresh(registry, entity, physics_system, temp_allocator, moving_layer);
    CharacterControllerState current = getState(registry, entity);

    CharacterMoveInput effective_input = input;
    if (!controller.input_enabled)
    {
        effective_input.move_forward = 0.0f;
        effective_input.move_right = 0.0f;
        effective_input.buttons = 0;
    }

    const glm::vec3 wish_dir = CharacterController::buildWishDirection(
        effective_input, current.grounded, current.ground_normal);
    const glm::vec3 target_horizontal = wish_dir * std::max(controller.move_speed, 0.0f);
    const bool jump = CharacterController::wantsJump(effective_input) && current.grounded;

    glm::vec3 new_velocity = current.velocity;
    if (current.grounded)
    {
        glm::vec3 relative_velocity = current.velocity - current.ground_velocity;
        relative_velocity.y = 0.0f;
        const float authority = glm::clamp(controller.ground_control, 0.0f, 1.0f);
        const glm::vec3 relative_horizontal = glm::mix(relative_velocity, target_horizontal, authority);

        new_velocity = current.ground_velocity;
        new_velocity.x += relative_horizontal.x;
        new_velocity.z += relative_horizontal.z;

        if (jump)
            new_velocity.y += std::max(controller.jump_velocity, 0.0f);
    }
    else
    {
        glm::vec3 current_horizontal(current.velocity.x, 0.0f, current.velocity.z);
        const float authority = glm::clamp(controller.air_control, 0.0f, 1.0f);
        const glm::vec3 blended_horizontal = glm::mix(current_horizontal, target_horizontal, authority);

        new_velocity.x = blended_horizontal.x;
        new_velocity.z = blended_horizontal.z;
    }

    new_velocity += gravity * 9.81f * controller.gravity_scale * delta_time;

    if (!isValidVec3(new_velocity))
        new_velocity = glm::vec3(0.0f);
    new_velocity = clampVelocity(new_velocity);

    const JPH::RVec3 start_position = character.GetPosition();
    character.SetLinearVelocity(toJolt(new_velocity));

    JPH::CharacterVirtual::ExtendedUpdateSettings update_settings;
    update_settings.mStickToFloorStepDown = JPH::Vec3(0.0f, -std::max(controller.stick_to_floor_distance, 0.0f), 0.0f);
    update_settings.mWalkStairsStepUp = JPH::Vec3(0.0f, std::max(controller.step_up_height, 0.0f), 0.0f);

    JPH::BodyFilter body_filter;
    JPH::ShapeFilter shape_filter;
    character.ExtendedUpdate(
        delta_time,
        toJolt(gravity * 9.81f * controller.gravity_scale),
        update_settings,
        physics_system.GetDefaultBroadPhaseLayerFilter(moving_layer),
        physics_system.GetDefaultLayerFilter(moving_layer),
        body_filter,
        shape_filter,
        temp_allocator);

    const JPH::RVec3 end_position = character.GetPosition();
    const glm::vec3 effective_velocity = toGlmR((end_position - start_position) / delta_time);
    character.SetLinearVelocity(toJolt(clampVelocity(effective_velocity)));

    CharacterControllerState result = getState(registry, entity);
    result.velocity = effective_velocity;
    syncCharacterStateToComponents(registry, entity, result);
    return result;
}
