#include "Assets/AssetManager.hpp"
#include "Components/Components.hpp"
#include "Graphics/HeadlessRenderAPI.hpp"
#include "LevelManager.hpp"
#include "PhysicsSystem.hpp"
#include "Utils/Log.hpp"
#include "world.hpp"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

static fs::path findRepoRoot()
{
    fs::path dir = fs::current_path();
    while (!dir.empty())
    {
        if (fs::exists(dir / "project.buildscript"))
            return dir;
        fs::path parent = dir.parent_path();
        if (parent == dir)
            break;
        dir = parent;
    }
    return fs::current_path();
}

static bool approx(float a, float b, float epsilon = 0.02f)
{
    return std::abs(a - b) <= epsilon;
}

static bool fail(const std::string& name, const std::string& reason)
{
    std::cerr << "[FAIL] " << name << ": " << reason << std::endl;
    return false;
}

static bool pass(const std::string& name)
{
    std::cout << "[PASS] " << name << std::endl;
    return true;
}

static float horizontalSpeed(const glm::vec3& v)
{
    return std::sqrt(v.x * v.x + v.z * v.z);
}

static CharacterController::MovementTuning makeSourceMovementTestTuning()
{
    CharacterController::MovementTuning tuning;
    tuning.max_speed = 10.0f;
    tuning.jump_velocity = 5.0f;
    tuning.gravity = glm::vec3(0.0f, -9.81f, 0.0f);
    tuning.fixed_delta = 1.0f / 60.0f;
    tuning.ground_acceleration = 5.5f;
    tuning.air_acceleration = 12.0f;
    tuning.friction = 5.2f;
    tuning.stop_speed = tuning.max_speed * 0.25f;
    tuning.air_wish_speed_cap = tuning.max_speed * (30.0f / 320.0f);
    tuning.surface_friction = 1.0f;
    tuning.max_velocity = 100.0f;
    return tuning;
}

static bool testSourceGroundAcceleration()
{
    const std::string name = "source movement ground acceleration";
    const auto tuning = makeSourceMovementTestTuning();
    CharacterControllerState state;
    state.grounded = true;

    CharacterMoveInput input;
    input.move_forward = 1.0f;

    const CharacterControllerState result =
        CharacterController::simulateSourceMovement(input, state, tuning);

    if (result.velocity.z <= 0.0f)
        return fail(name, "forward input did not accelerate");
    if (result.velocity.z >= tuning.max_speed)
        return fail(name, "ground movement snapped directly to max speed");

    const float expected = tuning.ground_acceleration * tuning.max_speed * tuning.fixed_delta;
    if (!approx(result.velocity.z, expected, 0.03f))
        return fail(name, "ground acceleration did not match Source-style formula");

    return pass(name);
}

static bool testSourceGroundFriction()
{
    const std::string name = "source movement ground friction";
    const auto tuning = makeSourceMovementTestTuning();
    CharacterControllerState state;
    state.grounded = true;
    state.velocity = glm::vec3(5.0f, 0.0f, 0.0f);

    const CharacterControllerState result =
        CharacterController::simulateSourceMovement(CharacterMoveInput{}, state, tuning);

    if (horizontalSpeed(result.velocity) >= horizontalSpeed(state.velocity))
        return fail(name, "ground friction did not slow horizontal velocity");

    const float expected = 5.0f - 5.0f * tuning.friction * tuning.fixed_delta;
    if (!approx(horizontalSpeed(result.velocity), expected, 0.03f))
        return fail(name, "ground friction did not match Source-style formula");

    return pass(name);
}

static bool testSourceJumpPreservesHorizontalVelocity()
{
    const std::string name = "source movement jump preserves horizontal velocity";
    const auto tuning = makeSourceMovementTestTuning();
    CharacterControllerState state;
    state.grounded = true;
    state.velocity = glm::vec3(3.0f, 0.0f, 0.0f);

    CharacterMoveInput input;
    input.buttons = CharacterMoveFlags::Jump;

    const CharacterControllerState result =
        CharacterController::simulateSourceMovement(input, state, tuning);

    if (!approx(result.velocity.x, 3.0f, 0.02f))
        return fail(name, "jump changed horizontal velocity");
    if (result.velocity.y < 4.7f)
        return fail(name, "jump impulse was not applied");
    if (result.grounded)
        return fail(name, "jump did not leave grounded state");

    return pass(name);
}

static bool testSourceAirAccelerationCap()
{
    const std::string name = "source movement air acceleration cap";
    const auto tuning = makeSourceMovementTestTuning();
    CharacterControllerState state;
    state.grounded = false;

    CharacterMoveInput input;
    input.move_forward = 1.0f;

    const CharacterControllerState result =
        CharacterController::simulateSourceMovement(input, state, tuning);

    if (result.velocity.z <= 0.0f)
        return fail(name, "air input did not accelerate");
    if (result.velocity.z > tuning.air_wish_speed_cap + 0.02f)
        return fail(name, "air acceleration exceeded wish-speed cap");
    if (result.velocity.z >= tuning.max_speed)
        return fail(name, "air movement snapped directly to max speed");

    return pass(name);
}

static bool testSourceDiagonalInputClampsSpeed()
{
    const std::string name = "source movement diagonal input clamps speed";
    const auto tuning = makeSourceMovementTestTuning();
    CharacterControllerState state;
    state.grounded = true;

    CharacterMoveInput input;
    input.move_forward = 1.0f;
    input.move_right = 1.0f;

    for (int i = 0; i < 120; ++i)
    {
        state = CharacterController::simulateSourceMovement(input, state, tuning);
        state.grounded = true;
    }

    if (horizontalSpeed(state.velocity) > tuning.max_speed + 0.02f)
        return fail(name, "diagonal input exceeded max movement speed");
    if (horizontalSpeed(state.velocity) < tuning.max_speed * 0.8f)
        return fail(name, "diagonal input never accelerated close to max speed");

    return pass(name);
}

static void run(const std::string& name)
{
    std::cout << "[RUN] " << name << std::endl;
}

static JPH::ShapeRefC makeBoxShape()
{
    ColliderComponent col;
    col.shape_type = ColliderShapeType::Box;
    col.box_half_extents = glm::vec3(0.5f);
    return PhysicsSystem::createShapeFromCollider(col, glm::vec3(1.0f));
}

static bool testDynamicGravityFlag()
{
    const std::string name = "dynamic gravity flag";
    world w;
    w.initializePhysics();
    w.setGravity(glm::vec3(0.0f, -1.0f, 0.0f));

    auto shape = makeBoxShape();
    if (!shape)
        return fail(name, "failed to create box shape");

    auto no_gravity = w.registry.create();
    w.registry.emplace<TransformComponent>(no_gravity, 0.0f, 10.0f, 0.0f);
    auto& rb0 = w.registry.emplace<RigidBodyComponent>(no_gravity);
    rb0.mass = 1.0f;
    rb0.apply_gravity = false;

    PhysicsSystem::PhysicsBodyDesc no_gravity_desc;
    no_gravity_desc.mass = rb0.mass;
    no_gravity_desc.apply_gravity = false;
    w.getPhysicsSystem().createDynamicBody(glm::vec3(0.0f, 10.0f, 0.0f), glm::vec3(0.0f), shape, no_gravity, no_gravity_desc);

    auto with_gravity = w.registry.create();
    w.registry.emplace<TransformComponent>(with_gravity, 0.0f, 10.0f, 0.0f);
    auto& rb1 = w.registry.emplace<RigidBodyComponent>(with_gravity);
    rb1.mass = 1.0f;
    rb1.apply_gravity = true;

    PhysicsSystem::PhysicsBodyDesc gravity_desc;
    gravity_desc.mass = rb1.mass;
    gravity_desc.apply_gravity = true;
    w.getPhysicsSystem().createDynamicBody(glm::vec3(0.0f, 10.0f, 0.0f), glm::vec3(0.0f), shape, with_gravity, gravity_desc);

    for (int i = 0; i < 20; ++i)
        w.getPhysicsSystem().stepPhysics(w.registry);

    float y0 = w.registry.get<TransformComponent>(no_gravity).position.y;
    float y1 = w.registry.get<TransformComponent>(with_gravity).position.y;

    if (!approx(y0, 10.0f))
        return fail(name, "apply_gravity=false body moved vertically");
    if (y1 >= 9.8f)
        return fail(name, "apply_gravity=true body did not fall");

    return pass(name);
}

static bool testPhysicsSettingsConfigureGravity()
{
    const std::string name = "physics settings configure gravity";

    PhysicsSystemSettings settings;
    settings.gravity_direction = glm::vec3(0.0f, -1.0f, 0.0f);
    settings.gravity_acceleration = 1.0f;

    world w(settings);
    w.initializePhysics();

    if (!approx(w.getPhysicsSystem().getGravityAcceleration(), 1.0f, 0.001f))
        return fail(name, "gravity acceleration setting was not applied");

    auto shape = makeBoxShape();
    if (!shape)
        return fail(name, "failed to create box shape");

    auto with_gravity = w.registry.create();
    w.registry.emplace<TransformComponent>(with_gravity, 0.0f, 10.0f, 0.0f);
    auto& rb = w.registry.emplace<RigidBodyComponent>(with_gravity);
    rb.mass = 1.0f;
    rb.apply_gravity = true;

    PhysicsSystem::PhysicsBodyDesc gravity_desc;
    gravity_desc.mass = rb.mass;
    gravity_desc.apply_gravity = true;
    w.getPhysicsSystem().createDynamicBody(glm::vec3(0.0f, 10.0f, 0.0f), glm::vec3(0.0f), shape, with_gravity, gravity_desc);

    for (int i = 0; i < 20; ++i)
        w.getPhysicsSystem().stepPhysics(w.registry);

    const float y = w.registry.get<TransformComponent>(with_gravity).position.y;
    if (y <= 9.8f)
        return fail(name, "custom gravity acceleration was ignored");
    if (y >= 10.0f)
        return fail(name, "body did not fall under custom gravity");

    return pass(name);
}

static bool testPlayerBodyDoesNotUseJoltGravity()
{
    const std::string name = "player body gravity ownership";
    world w;
    w.initializePhysics();
    w.setGravity(glm::vec3(0.0f, -1.0f, 0.0f));

    auto player = w.registry.create();
    w.registry.emplace<TransformComponent>(player, 0.0f, 5.0f, 0.0f);
    w.registry.emplace<PlayerComponent>(player);
    auto& rb = w.registry.emplace<RigidBodyComponent>(player);
    rb.mass = 80.0f;
    rb.apply_gravity = true;

    if (w.getPhysicsSystem().createPlayerBody(w.registry, player).IsInvalid())
        return fail(name, "failed to create player body");

    for (int i = 0; i < 20; ++i)
        w.getPhysicsSystem().stepPhysics(w.registry);

    float y = w.registry.get<TransformComponent>(player).position.y;
    if (!approx(y, 5.0f))
        return fail(name, "Jolt gravity moved the player body");

    return pass(name);
}

static bool testNetworkSpawnedPlayerGroundsOnMesh(const fs::path& repo_root)
{
    const std::string name = "network-spawned player grounds on mesh";
    HeadlessRenderAPI render_api;
    Assets::AssetManager::get().setAssetRoot((repo_root / "Templates" / "FPSShooter" / "assets").string());
    Assets::AssetManager::get().setAssetPrefix("assets");
    Assets::AssetManager::get().initialize(&render_api);

    LevelData data;
    data.metadata.level_name = "PhysicsTest";
    data.entities.clear();

    LevelEntity ground;
    ground.name = "ground";
    ground.type = EntityType::Renderable;
    ground.mesh_path = "assets/models/ground_plane.obj";
    ground.use_mesh_collision = true;
    data.entities.push_back(ground);

    world w;
    w.initializePhysics();

    LevelManager level_manager;
    if (!level_manager.instantiateLevel(data, w, &render_api))
        return fail(name, "instantiateLevel returned false");

    auto player = w.registry.create();
    w.registry.emplace<TransformComponent>(player, 0.0f, 3.0f, 0.0f);
    auto& pc = w.registry.emplace<PlayerComponent>(player);
    pc.speed = 10.0f;
    pc.jump_force = 5.0f;

    auto& rb = w.registry.emplace<RigidBodyComponent>(player);
    rb.mass = 80.0f;
    rb.apply_gravity = false;
    rb.velocity = glm::vec3(0.0f, -5.0f, 0.0f);

    if (w.getPhysicsSystem().createPlayerBody(w.registry, player).IsInvalid())
        return fail(name, "failed to create player body");

    for (int i = 0; i < 90; ++i)
    {
        CharacterMoveInput input;
        w.simulate_character_controller(player, input, w.fixed_delta);
        w.getPhysicsSystem().stepPhysics(w.registry);
        w.player_collisions(player);
    }

    const auto& transform = w.registry.get<TransformComponent>(player);
    const auto& final_pc = w.registry.get<PlayerComponent>(player);
    if (transform.position.y < 1.0f)
        return fail(name, "player fell through mesh collider");
    if (!final_pc.grounded)
        return fail(name, "player was not grounded on mesh collider");
    if (final_pc.ground_normal.y < 0.8f)
        return fail(name, "player ground normal is not upward");

    return pass(name);
}

static bool testLevelPlayerCreatesCharacterController()
{
    const std::string name = "level player creates character controller";

    LevelData data;
    data.metadata.level_name = "CharacterControllerTest";
    data.entities.clear();

    LevelEntity player_data;
    player_data.name = "player";
    player_data.type = EntityType::Player;
    player_data.position = glm::vec3(0.0f, 2.0f, 0.0f);
    player_data.speed = 7.0f;
    player_data.jump_force = 4.0f;
    data.entities.push_back(player_data);

    world w;
    w.initializePhysics();

    LevelManager level_manager;
    if (!level_manager.instantiateLevel(data, w, nullptr))
        return fail(name, "instantiateLevel returned false");

    entt::entity player = entt::null;
    auto view = w.registry.view<PlayerComponent>();
    for (auto entity : view)
    {
        player = entity;
        break;
    }

    if (player == entt::null)
        return fail(name, "player entity was not created");
    if (!w.registry.all_of<CharacterControllerComponent>(player))
        return fail(name, "player has no CharacterControllerComponent");
    if (!w.getPhysicsSystem().hasCharacterController(player))
        return fail(name, "player has no runtime character controller");

    const auto& controller = w.registry.get<CharacterControllerComponent>(player);
    if (!approx(controller.move_speed, 7.0f) || !approx(controller.jump_velocity, 4.0f))
        return fail(name, "player movement settings were not copied to controller");

    return pass(name);
}

static bool testUseMeshCollisionCreatesBody(const fs::path& repo_root)
{
    const std::string name = "use_mesh_collision creates body";
    HeadlessRenderAPI render_api;
    Assets::AssetManager::get().setAssetRoot((repo_root / "Templates" / "FPSShooter" / "assets").string());
    Assets::AssetManager::get().setAssetPrefix("assets");
    Assets::AssetManager::get().initialize(&render_api);

    LevelData data;
    data.metadata.level_name = "PhysicsTest";
    data.entities.clear();

    LevelEntity ground;
    ground.name = "ground";
    ground.type = EntityType::Renderable;
    ground.mesh_path = "assets/models/ground_plane.obj";
    ground.use_mesh_collision = true;
    data.entities.push_back(ground);

    world w;
    w.initializePhysics();

    LevelManager level_manager;
    if (!level_manager.instantiateLevel(data, w, &render_api))
        return fail(name, "instantiateLevel returned false");

    entt::entity ground_entity = entt::null;
    auto tags = w.registry.view<TagComponent>();
    for (auto entity : tags)
    {
        if (tags.get<TagComponent>(entity).name == "ground")
        {
            ground_entity = entity;
            break;
        }
    }

    if (ground_entity == entt::null)
        return fail(name, "ground entity was not created");
    if (!w.registry.all_of<ColliderComponent>(ground_entity))
        return fail(name, "ground has no ColliderComponent");
    if (!w.getPhysicsSystem().hasBody(ground_entity))
        return fail(name, "ground collider did not create a Jolt body");

    glm::vec3 hit_point, hit_normal;
    if (!w.raycast(glm::vec3(0.0f, 5.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 20.0f, hit_point, hit_normal))
        return fail(name, "downward raycast did not hit mesh collider");
    if (hit_normal.y < 0.8f)
        return fail(name, "mesh collider hit normal is not upward");

    return pass(name);
}

static bool testFpsShooterLevelReferences(const fs::path& repo_root)
{
    const std::string name = "FPSShooter level references";
    Assets::AssetManager::get().setAssetRoot((repo_root / "Templates" / "FPSShooter" / "assets").string());
    Assets::AssetManager::get().setAssetPrefix("assets");

    LevelManager level_manager;
    LevelData main_level;
    fs::path main_path = repo_root / "Templates" / "FPSShooter" / "assets" / "levels" / "main.level.json";
    if (!level_manager.loadLevel(main_path.string(), main_level))
        return fail(name, "failed to parse main.level.json");

    bool saw_map = false;
    for (const LevelEntity& entity : main_level.entities)
    {
        if (!entity.mesh_path.empty())
        {
            std::string resolved = Assets::AssetManager::get().resolveAssetPath(entity.mesh_path);
            if (!fs::exists(resolved))
                return fail(name, "missing mesh asset: " + entity.mesh_path);
        }
        if (!entity.collider_mesh_path.empty())
        {
            std::string resolved = Assets::AssetManager::get().resolveAssetPath(entity.collider_mesh_path);
            if (!fs::exists(resolved))
                return fail(name, "missing collider asset: " + entity.collider_mesh_path);
        }
        if (entity.name == "map_ground")
        {
            saw_map = true;
            if (!entity.use_mesh_collision)
                return fail(name, "map does not enable use_mesh_collision");
            if (!entity.collider_mesh_path.empty())
                return fail(name, "map still references a separate collider mesh");
        }
    }
    if (!saw_map)
        return fail(name, "main level has no map entity");

    LevelData physics_level;
    fs::path physics_path = repo_root / "Templates" / "FPSShooter" / "assets" / "levels" / "physics.level.json";
    if (!level_manager.loadLevel(physics_path.string(), physics_level))
        return fail(name, "failed to parse physics.level.json");

    for (const LevelEntity& entity : physics_level.entities)
    {
        if (!entity.mesh_path.empty())
        {
            std::string resolved = Assets::AssetManager::get().resolveAssetPath(entity.mesh_path);
            if (!fs::exists(resolved))
                return fail(name, "missing physics-level mesh asset: " + entity.mesh_path);
        }
    }

    return pass(name);
}

int main()
{
    EE::CLog::Init();
    fs::path repo_root = findRepoRoot();
    bool ok = true;
    run("source movement ground acceleration");
    ok = testSourceGroundAcceleration() && ok;
    run("source movement ground friction");
    ok = testSourceGroundFriction() && ok;
    run("source movement jump preserves horizontal velocity");
    ok = testSourceJumpPreservesHorizontalVelocity() && ok;
    run("source movement air acceleration cap");
    ok = testSourceAirAccelerationCap() && ok;
    run("source movement diagonal input clamps speed");
    ok = testSourceDiagonalInputClampsSpeed() && ok;
    run("dynamic gravity flag");
    ok = testDynamicGravityFlag() && ok;
    run("physics settings configure gravity");
    ok = testPhysicsSettingsConfigureGravity() && ok;
    run("player body gravity ownership");
    ok = testPlayerBodyDoesNotUseJoltGravity() && ok;
    run("network-spawned player grounds on mesh");
    ok = testNetworkSpawnedPlayerGroundsOnMesh(repo_root) && ok;
    run("level player creates character controller");
    ok = testLevelPlayerCreatesCharacterController() && ok;
    run("use_mesh_collision creates body");
    ok = testUseMeshCollisionCreatesBody(repo_root) && ok;
    run("FPSShooter level references");
    ok = testFpsShooterLevelReferences(repo_root) && ok;

    Assets::AssetManager::get().shutdown();
    EE::CLog::Shutdown();
    return ok ? 0 : 1;
}
