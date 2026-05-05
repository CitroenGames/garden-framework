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
    run("dynamic gravity flag");
    ok = testDynamicGravityFlag() && ok;
    run("player body gravity ownership");
    ok = testPlayerBodyDoesNotUseJoltGravity() && ok;
    run("network-spawned player grounds on mesh");
    ok = testNetworkSpawnedPlayerGroundsOnMesh(repo_root) && ok;
    run("use_mesh_collision creates body");
    ok = testUseMeshCollisionCreatesBody(repo_root) && ok;
    run("FPSShooter level references");
    ok = testFpsShooterLevelReferences(repo_root) && ok;

    Assets::AssetManager::get().shutdown();
    EE::CLog::Shutdown();
    return ok ? 0 : 1;
}
