#include "Assets/TerrainBuilder.hpp"
#include "Assets/AssetManager.hpp"
#include "Components/Components.hpp"
#include "Graphics/HeadlessRenderAPI.hpp"
#include "LevelManager.hpp"
#include "PhysicsSystem.hpp"
#include "PlayerController.hpp"
#include "Reflection/EngineReflection.hpp"
#include "Reflection/ReflectionRegistry.hpp"
#include "Utils/Log.hpp"
#include "world.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

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

static void appendLe16(std::vector<std::uint8_t>& out, std::uint16_t value)
{
    out.push_back(static_cast<std::uint8_t>(value & 0xff));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
}

static void appendLe32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>(value & 0xff));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
}

static bool writeTestHeightmapBmp(const fs::path& path)
{
    constexpr int width = 4;
    constexpr int height = 4;
    const int row_stride = ((width * 3 + 3) / 4) * 4;
    const std::uint32_t pixel_bytes = static_cast<std::uint32_t>(row_stride * height);
    const std::uint32_t file_size = 14 + 40 + pixel_bytes;

    std::vector<std::uint8_t> bytes;
    bytes.reserve(file_size);
    bytes.push_back('B');
    bytes.push_back('M');
    appendLe32(bytes, file_size);
    appendLe16(bytes, 0);
    appendLe16(bytes, 0);
    appendLe32(bytes, 54);
    appendLe32(bytes, 40);
    appendLe32(bytes, width);
    appendLe32(bytes, height);
    appendLe16(bytes, 1);
    appendLe16(bytes, 24);
    appendLe32(bytes, 0);
    appendLe32(bytes, pixel_bytes);
    appendLe32(bytes, 2835);
    appendLe32(bytes, 2835);
    appendLe32(bytes, 0);
    appendLe32(bytes, 0);

    std::vector<std::uint8_t> pixels(pixel_bytes, 0);
    for (int y = 0; y < height; ++y)
    {
        const int bmp_y = height - 1 - y;
        for (int x = 0; x < width; ++x)
        {
            const std::uint8_t v = static_cast<std::uint8_t>(((x + y) * 255) / (width + height - 2));
            const int offset = bmp_y * row_stride + x * 3;
            pixels[offset + 0] = v;
            pixels[offset + 1] = v;
            pixels[offset + 2] = v;
        }
    }
    bytes.insert(bytes.end(), pixels.begin(), pixels.end());

    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    if (!file)
        return false;
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return file.good();
}

static float meshMaxY(const mesh& m)
{
    float max_y = -1000000.0f;
    for (size_t i = 0; i < m.vertices_len; ++i)
        max_y = std::max(max_y, m.vertices[i].vy);
    return max_y;
}

static float meshMinY(const mesh& m)
{
    float min_y = 1000000.0f;
    for (size_t i = 0; i < m.vertices_len; ++i)
        min_y = std::min(min_y, m.vertices[i].vy);
    return min_y;
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

static bool testSourceWaterMovement()
{
    const std::string name = "source movement water swimming";
    auto tuning = makeSourceMovementTestTuning();
    tuning.water_level = CharacterWaterLevel::Waist;
    tuning.water_speed_scale = 0.8f;
    tuning.water_acceleration = 5.5f;
    tuning.water_friction = 5.2f;
    tuning.water_sink_speed = 1.75f;

    CharacterControllerState state;
    state.grounded = true;
    state.water_level = CharacterWaterLevel::Waist;
    state.velocity = glm::vec3(4.0f, 0.0f, 0.0f);

    const CharacterControllerState idle =
        CharacterController::simulateSourceMovement(CharacterMoveInput{}, state, tuning);

    if (idle.grounded)
        return fail(name, "water movement stayed grounded");
    if (idle.water_level != CharacterWaterLevel::Waist)
        return fail(name, "water level was not preserved");
    if (glm::length(idle.velocity) >= glm::length(state.velocity))
        return fail(name, "water friction did not slow existing velocity");
    if (idle.velocity.y >= 0.0f)
        return fail(name, "idle water movement did not sink");

    CharacterMoveInput jump_input;
    jump_input.buttons = CharacterMoveFlags::Jump;
    CharacterControllerState still_water = state;
    still_water.velocity = glm::vec3(0.0f);
    const CharacterControllerState swim_up =
        CharacterController::simulateSourceMovement(jump_input, still_water, tuning);

    if (swim_up.velocity.y <= 0.0f)
        return fail(name, "jump did not swim upward in water");
    if (swim_up.velocity.y >= tuning.jump_velocity)
        return fail(name, "water jump used full ground jump impulse");

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

static bool testWaterComponentProvidesSwimmingVolume()
{
    const std::string name = "water component provides swimming volume";

    world w;
    w.initializePhysics();

    auto water_entity = w.registry.create();
    w.registry.emplace<TransformComponent>(water_entity, 0.0f, 0.0f, 0.0f);
    auto& water = w.registry.emplace<WaterComponent>(water_entity);
    water.half_extents = glm::vec3(8.0f, 4.0f, 8.0f);
    water.surface_offset = 0.25f;
    water.swim_speed_scale = 0.55f;

    auto player = w.registry.create();
    w.registry.emplace<TransformComponent>(player, 0.0f, 0.0f, 0.0f);
    auto& pc = w.registry.emplace<PlayerComponent>(player);
    pc.speed = 10.0f;
    pc.jump_force = 5.0f;
    auto& rb = w.registry.emplace<RigidBodyComponent>(player);
    rb.mass = 80.0f;
    rb.apply_gravity = false;

    if (w.getPhysicsSystem().createPlayerBody(w.registry, player).IsInvalid())
        return fail(name, "failed to create player body");

    CharacterMoveInput input;
    const CharacterControllerState result =
        w.simulate_character_controller(player, input, w.fixed_delta);

    if (result.water_level < CharacterWaterLevel::Waist)
        return fail(name, "character state did not enter swimming water level");
    const auto& controller = w.registry.get<CharacterControllerComponent>(player);
    if (!controller.swimming)
        return fail(name, "CharacterControllerComponent swimming state was not synced");

    water.affects_swimming = false;
    const CharacterControllerState disabled_result =
        w.simulate_character_controller(player, input, w.fixed_delta);
    if (disabled_result.water_level != CharacterWaterLevel::None)
        return fail(name, "disabled WaterComponent still affected swimming");

    return pass(name);
}

static bool testCameraSpringComponentOffsetsCamera()
{
    const std::string name = "camera spring offsets camera";
    world w;

    entt::entity player = w.registry.create();
    auto& transform = w.registry.emplace<TransformComponent>(player, 0.0f, 1.0f, 0.0f);
    w.registry.emplace<PlayerComponent>(player);
    auto& spring = w.registry.emplace<CameraSpringComponent>(player);
    spring.pivot_offset = glm::vec3(0.0f, 1.0f, 0.0f);
    spring.local_offset = glm::vec3(0.5f, 0.0f, -4.0f);
    spring.position_lag_enabled = false;
    spring.collision_enabled = false;
    spring.rotate_target_yaw = true;

    w.world_camera.position = glm::vec3(10.0f, 10.0f, 10.0f);
    w.world_camera.rotation = glm::vec3(0.0f);

    PlayerController controller(nullptr, &w);
    controller.setPossessedPlayer(player);
    controller.setMovementEnabled(false);
    controller.update(1.0f / 60.0f);

    const glm::vec3 expected_position(0.5f, 2.0f, -4.0f);
    if (glm::distance(w.world_camera.position, expected_position) > 0.01f)
        return fail(name, "camera did not snap to the configured third-person arm");

    w.world_camera.rotation.y = 1.0f;
    controller.update(1.0f / 60.0f);
    if (!approx(transform.rotation.y, glm::degrees(1.0f), 0.01f))
        return fail(name, "target yaw did not follow camera yaw");

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

static bool testTerrainBuilderHeightmapMesh(const fs::path& repo_root)
{
    const std::string name = "terrain builder heightmap mesh";
    const fs::path heightmap_path = repo_root / "build" / "test_assets" / "terrain_heightmap.bmp";
    if (!writeTestHeightmapBmp(heightmap_path))
        return fail(name, "failed to write test heightmap");

    TerrainComponent terrain;
    terrain.width = 4.0f;
    terrain.depth = 4.0f;
    terrain.height_scale = 3.0f;
    terrain.height_offset = -1.0f;
    terrain.sample_step = 1;

    Assets::TerrainBuildResult cpu_result =
        Assets::TerrainBuilder::buildFromHeightmap(terrain, heightmap_path.string(), false);
    if (!cpu_result.success)
        return fail(name, cpu_result.error_message);
    if (!cpu_result.render_mesh || !cpu_result.collision_mesh)
        return fail(name, "builder did not return both meshes");
    if (cpu_result.render_mesh->vertices_len != 54)
        return fail(name, "unexpected terrain triangle count");
    if (meshMaxY(*cpu_result.render_mesh) <= meshMinY(*cpu_result.render_mesh))
        return fail(name, "CPU render mesh was not displaced");

    Assets::TerrainBuildResult gpu_result =
        Assets::TerrainBuilder::buildFromHeightmap(terrain, heightmap_path.string(), true);
    if (!gpu_result.success)
        return fail(name, gpu_result.error_message);
    if (std::abs(meshMaxY(*gpu_result.render_mesh)) > 0.001f ||
        std::abs(meshMinY(*gpu_result.render_mesh)) > 0.001f)
        return fail(name, "GPU-displaced render mesh should stay flat on CPU");
    if (meshMaxY(*gpu_result.collision_mesh) <= meshMinY(*gpu_result.collision_mesh))
        return fail(name, "collision mesh was not displaced");
    if (gpu_result.render_mesh->aabb_max.y < 1.9f)
        return fail(name, "render mesh bounds did not include displaced height");

    return pass(name);
}

static bool testLevelTerrainCreatesMeshAndCollision(const fs::path& repo_root)
{
    const std::string name = "level terrain creates mesh and collision";
    const fs::path asset_root = repo_root / "build" / "test_assets";
    const fs::path heightmap_path = asset_root / "terrain_level_heightmap.bmp";
    if (!writeTestHeightmapBmp(heightmap_path))
        return fail(name, "failed to write test heightmap");

    HeadlessRenderAPI render_api;
    Assets::AssetManager::get().setAssetRoot(asset_root.string());
    Assets::AssetManager::get().setAssetPrefix("assets");
    Assets::AssetManager::get().initialize(&render_api);

    LevelData data;
    data.metadata.level_name = "TerrainLevelTest";
    data.entities.clear();

    LevelEntity terrain_entity;
    terrain_entity.name = "terrain";
    terrain_entity.type = EntityType::Static;
    terrain_entity.reflected_components = nlohmann::json::object({
        {"TerrainComponent", nlohmann::json::object({
            {"heightmap_path", "assets/terrain_level_heightmap.bmp"},
            {"width", 6.0f},
            {"depth", 6.0f},
            {"height_scale", 2.0f},
            {"height_offset", 0.0f},
            {"sample_step", 1},
            {"gpu_displacement", true},
            {"generate_collision", true},
            {"friction", 0.8f},
            {"restitution", 0.0f}
        })}
    });
    data.entities.push_back(terrain_entity);

    world w;
    w.initializePhysics();

    ReflectionRegistry reflection;
    registerEngineReflection(reflection);

    LevelManager level_manager;
    level_manager.setReflectionRegistry(&reflection);
    if (!level_manager.instantiateLevel(data, w, &render_api))
        return fail(name, "instantiateLevel returned false");

    entt::entity terrain = entt::null;
    auto tags = w.registry.view<TagComponent>();
    for (auto entity : tags)
    {
        if (tags.get<TagComponent>(entity).name == "terrain")
        {
            terrain = entity;
            break;
        }
    }

    if (terrain == entt::null)
        return fail(name, "terrain entity was not created");
    if (!w.registry.all_of<MeshComponent>(terrain))
        return fail(name, "terrain has no MeshComponent");
    if (!w.registry.all_of<ColliderComponent>(terrain))
        return fail(name, "terrain has no ColliderComponent");
    if (!w.getPhysicsSystem().hasBody(terrain))
        return fail(name, "terrain collider did not create a physics body");

    const auto& mesh_component = w.registry.get<MeshComponent>(terrain);
    if (!mesh_component.m_mesh || mesh_component.m_mesh->vertices_len == 0)
        return fail(name, "terrain render mesh is empty");

    const auto& collider = w.registry.get<ColliderComponent>(terrain);
    if (collider.shape_type != ColliderShapeType::Mesh || !collider.is_mesh_valid())
        return fail(name, "terrain collider is not a valid mesh collider");

    glm::vec3 hit_point, hit_normal;
    if (!w.raycast(glm::vec3(0.0f, 8.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 20.0f, hit_point, hit_normal))
        return fail(name, "downward raycast did not hit terrain collider");
    if (hit_normal.y < 0.4f)
        return fail(name, "terrain hit normal is not sufficiently upward");

    return pass(name);
}

static bool testRaycastClosestCanIgnoreShooterBody()
{
    const std::string name = "raycast closest ignores shooter body";
    world w;
    w.initializePhysics();

    auto player = w.registry.create();
    w.registry.emplace<TransformComponent>(player, 0.0f, 0.6f, 0.0f);
    w.registry.emplace<PlayerComponent>(player);
    auto& rb = w.registry.emplace<RigidBodyComponent>(player);
    rb.mass = 80.0f;
    rb.apply_gravity = false;

    if (w.getPhysicsSystem().createPlayerBody(w.registry, player).IsInvalid())
        return fail(name, "failed to create player body");

    auto wall = w.registry.create();
    w.registry.emplace<TransformComponent>(wall, 0.0f, 0.6f, 5.0f);
    auto wall_shape = makeBoxShape();
    if (!wall_shape)
        return fail(name, "failed to create wall shape");
    if (w.getPhysicsSystem().createStaticBody(glm::vec3(0.0f, 0.6f, 5.0f), glm::vec3(0.0f), wall_shape, wall).IsInvalid())
        return fail(name, "failed to create wall body");

    PhysicsSystem::RaycastResult hit = w.raycastClosest(
        glm::vec3(0.0f, 0.6f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), 10.0f, player);

    if (!hit.hit)
        return fail(name, "raycast did not hit the wall");
    if (hit.entity != wall)
        return fail(name, "raycast hit the ignored player instead of the wall");
    if (!approx(hit.distance, 4.5f, 0.08f))
        return fail(name, "raycast hit distance was not the wall front face");

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

    LevelData terrain_level;
    fs::path terrain_path = repo_root / "Templates" / "FPSShooter" / "assets" / "levels" / "terrain_showcase.level.json";
    if (!level_manager.loadLevel(terrain_path.string(), terrain_level))
        return fail(name, "failed to parse terrain_showcase.level.json");

    bool saw_terrain = false;
    for (const LevelEntity& entity : terrain_level.entities)
    {
        const auto& components = entity.reflected_components;
        if (!components.is_object() || !components.contains("TerrainComponent"))
            continue;

        saw_terrain = true;
        const auto& terrain = components["TerrainComponent"];
        for (const char* key : {"heightmap_path", "albedo_path"})
        {
            if (!terrain.contains(key) || !terrain[key].is_string() || terrain[key].get<std::string>().empty())
                return fail(name, std::string("terrain showcase missing ") + key);
            const std::string resolved = Assets::AssetManager::get().resolveAssetPath(terrain[key].get<std::string>());
            if (!fs::exists(resolved))
                return fail(name, std::string("missing terrain asset: ") + terrain[key].get<std::string>());
        }
    }
    if (!saw_terrain)
        return fail(name, "terrain showcase has no TerrainComponent");

    return pass(name);
}

static bool testThirdPersonTemplateReferences(const fs::path& repo_root)
{
    const std::string name = "ThirdPerson template references";
    Assets::AssetManager::get().setAssetRoot((repo_root / "Templates" / "ThirdPerson" / "assets").string());
    Assets::AssetManager::get().setAssetPrefix("assets");

    LevelManager level_manager;
    LevelData level;
    fs::path level_path = repo_root / "Templates" / "ThirdPerson" / "assets" / "levels" / "main.level.json";
    if (!level_manager.loadLevel(level_path.string(), level))
        return fail(name, "failed to parse main.level.json");

    bool saw_camera_spring = false;
    bool saw_visible_player_rep = false;

    for (const LevelEntity& entity : level.entities)
    {
        if (!entity.mesh_path.empty())
        {
            const std::string resolved = Assets::AssetManager::get().resolveAssetPath(entity.mesh_path);
            if (!fs::exists(resolved))
                return fail(name, "missing mesh asset: " + entity.mesh_path);
        }

        const auto& components = entity.reflected_components;
        if (!components.is_object())
            continue;

        if (components.contains("CameraSpringComponent"))
        {
            saw_camera_spring = true;
            const auto& spring = components["CameraSpringComponent"];
            if (!spring.value("enabled", false))
                return fail(name, "CameraSpringComponent is not enabled");
            if (!spring.contains("local_offset") || spring["local_offset"].value("z", 0.0f) >= 0.0f)
                return fail(name, "CameraSpringComponent does not place the camera behind the player");
        }

        if (components.contains("PlayerRepresentationComponent"))
        {
            const auto& rep = components["PlayerRepresentationComponent"];
            if (!rep.value("visible_only_freecam", true))
                saw_visible_player_rep = true;
        }
    }

    if (!saw_camera_spring)
        return fail(name, "template player has no CameraSpringComponent");
    if (!saw_visible_player_rep)
        return fail(name, "template has no visible third-person player representation");

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
    run("source movement water swimming");
    ok = testSourceWaterMovement() && ok;
    run("dynamic gravity flag");
    ok = testDynamicGravityFlag() && ok;
    run("physics settings configure gravity");
    ok = testPhysicsSettingsConfigureGravity() && ok;
    run("player body gravity ownership");
    ok = testPlayerBodyDoesNotUseJoltGravity() && ok;
    run("water component provides swimming volume");
    ok = testWaterComponentProvidesSwimmingVolume() && ok;
    run("camera spring offsets camera");
    ok = testCameraSpringComponentOffsetsCamera() && ok;
    run("network-spawned player grounds on mesh");
    ok = testNetworkSpawnedPlayerGroundsOnMesh(repo_root) && ok;
    run("level player creates character controller");
    ok = testLevelPlayerCreatesCharacterController() && ok;
    run("use_mesh_collision creates body");
    ok = testUseMeshCollisionCreatesBody(repo_root) && ok;
    run("terrain builder heightmap mesh");
    ok = testTerrainBuilderHeightmapMesh(repo_root) && ok;
    run("level terrain creates mesh and collision");
    ok = testLevelTerrainCreatesMeshAndCollision(repo_root) && ok;
    run("raycast closest ignores shooter body");
    ok = testRaycastClosestCanIgnoreShooterBody() && ok;
    run("FPSShooter level references");
    ok = testFpsShooterLevelReferences(repo_root) && ok;
    run("ThirdPerson template references");
    ok = testThirdPersonTemplateReferences(repo_root) && ok;

    Assets::AssetManager::get().shutdown();
    EE::CLog::Shutdown();
    return ok ? 0 : 1;
}
