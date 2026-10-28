#include "Character/CharacterPhysicsService.hpp"
#include "Assets/TerrainBuilder.hpp"
#include "Assets/AssetManager.hpp"
#include "Assets/AssetScanner.hpp"
#include "Assets/CookedCollisionSerializer.hpp"
#include "Components/Components.hpp"
#include "Components/HierarchyComponents.hpp"
#include "Components/IdentityComponent.hpp"
#include "ECS/EntityLifecycle.hpp"
#include "ECS/Hierarchy.hpp"
#include "Graphics/HeadlessRenderAPI.hpp"
#include "LevelManager.hpp"
#include "PhysicsSystem.hpp"
#include "Events/EventBus.hpp"
#include "Events/EngineEvents.hpp"
#include "Physics/PhysicsSettings.hpp"
#include "Network/PredictionTypes.hpp"
#include "PlayerController.hpp"
#include "Character/FpsCharacter.hpp"
#include "Reflection/EngineReflection.hpp"
#include "Reflection/ReflectionRegistry.hpp"
#include "Reflection/ReflectionSerializer.hpp"
#include "Tick/TickSystem.hpp"
#include "Utils/Log.hpp"
#include "world.hpp"

#include "spdlog/sinks/callback_sink.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <iterator>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <glm/gtc/constants.hpp>

namespace fs = std::filesystem;

// Test fixture bridge for raw entities created by the legacy physics setup.
namespace
{
    namespace PhysicsTestEcsBridge
    {
        EntityRef entityRef(world& test_world, entt::entity entity)
        {
            return ensureEntityRef(test_world.registry, entity);
        }
    }
}

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

static bool testFixedTickAccumulatorCarriesPartialTime()
{
    const std::string name = "fixed tick accumulator carries partial time";
    Tick::FixedTickAccumulator ticks(0.1f, 8);

    if (ticks.consume(0.05f) != 0)
        return fail(name, "partial delta consumed a tick");
    if (!approx(ticks.getAlpha(), 0.5f, 0.001f))
        return fail(name, "partial delta did not produce expected interpolation alpha");
    if (ticks.consume(0.05f) != 1)
        return fail(name, "second partial delta did not complete one tick");
    if (!approx(ticks.getAccumulatedTime(), 0.0f, 0.001f))
        return fail(name, "completed tick left unexpected accumulated time");

    return pass(name);
}

static bool testFixedTickAccumulatorCapsCatchUp()
{
    const std::string name = "fixed tick accumulator caps catch-up";
    Tick::FixedTickAccumulator ticks(0.1f, 3);

    if (ticks.consume(1.0f) != 3)
        return fail(name, "large delta was not capped to max tick steps");
    if (!approx(ticks.getAccumulatedTime(), 0.0f, 0.001f))
        return fail(name, "capped catch-up retained dropped time");

    return pass(name);
}

static bool testWorldSimulationTickTracksPhysicsSteps()
{
    const std::string name = "world simulation tick tracks physics steps";
    PhysicsSystemSettings settings;
    settings.fixed_delta = 0.1f;
    world w(settings);

    if (w.getSimulationTick() != 0)
        return fail(name, "new world did not start at tick zero");
    if (w.step_physics(0.05f) != 0)
        return fail(name, "partial delta consumed a physics step");
    if (w.getSimulationTick() != 0)
        return fail(name, "partial delta advanced simulation tick");
    if (!approx(w.getPhysicsInterpolationAlpha(), 0.5f, 0.001f))
        return fail(name, "partial delta did not expose expected interpolation alpha");

    if (w.step_physics(0.25f) != 3)
        return fail(name, "completed delta did not consume expected physics steps");
    if (w.getSimulationTick() != 3)
        return fail(name, "simulation tick did not match consumed physics steps");

    settings.fixed_delta = 0.2f;
    if (!w.configurePhysics(settings))
        return fail(name, "pre-initialize physics reconfigure failed");
    if (w.getSimulationTick() != 0)
        return fail(name, "physics reconfigure did not reset simulation tick");

    return pass(name);
}

static bool testWorldTimeTracksPhysicsSteps()
{
    const std::string name = "world time tracks physics steps";
    PhysicsSystemSettings settings;
    settings.fixed_delta = 0.1f;
    world w(settings);

    if (!approx(static_cast<float>(w.getWorldTime()), 0.0f, 0.0001f))
        return fail(name, "new world did not start at time zero");

    w.step_physics(0.05f);
    if (!approx(static_cast<float>(w.getWorldTime()), 0.0f, 0.0001f))
        return fail(name, "partial delta advanced world time");

    w.step_physics(0.25f);
    if (w.getSimulationTick() != 3)
        return fail(name, "simulation tick did not match consumed physics steps");
    if (!approx(static_cast<float>(w.getWorldTime()), 0.3f, 0.001f))
        return fail(name, "world time did not match consumed physics steps");

    if (!w.configurePhysics(settings))
        return fail(name, "pre-initialize physics reconfigure failed");
    if (!approx(static_cast<float>(w.getWorldTime()), 0.0f, 0.0001f))
        return fail(name, "physics reconfigure did not reset world time");

    return pass(name);
}

static bool testFrameSplitKeepsDeterministicTickAndTime()
{
    const std::string name = "frame split keeps deterministic tick and time";
    PhysicsSystemSettings settings;
    settings.fixed_delta = 0.1f;

    world single_call(settings);
    world split_calls(settings);

    single_call.step_physics(0.3f);
    split_calls.step_physics(0.1f);
    split_calls.step_physics(0.1f);
    split_calls.step_physics(0.1f);

    if (single_call.getSimulationTick() != split_calls.getSimulationTick())
        return fail(name, "same total frame time produced different tick counts");
    if (!approx(static_cast<float>(single_call.getWorldTime()),
                static_cast<float>(split_calls.getWorldTime()), 0.0001f))
        return fail(name, "same total frame time produced different world times");
    if (single_call.getSimulationTick() != 3)
        return fail(name, "expected three fixed steps for 0.3s at 10Hz");

    return pass(name);
}

static bool testTickContextNamesSubsteps()
{
    const std::string name = "tick context names substeps";
    PhysicsSystemSettings settings;
    settings.fixed_delta = 0.1f;
    world w(settings);

    Tick::SimulationTickContext first = w.getTickContext(0, 3);
    Tick::SimulationTickContext second = w.getTickContext(1, 3);
    Tick::SimulationTickContext third = w.getTickContext(2, 3);

    if (first.tick_index + 1 != second.tick_index || second.tick_index + 1 != third.tick_index)
        return fail(name, "substep tick indices are not contiguous");
    if (!approx(first.fixed_delta, 0.1f, 0.0001f))
        return fail(name, "tick context did not carry the fixed delta");
    if (!approx(static_cast<float>(third.world_time - first.world_time), 0.2f, 0.0001f))
        return fail(name, "substep world times are not spaced by the fixed delta");

    w.step_physics(0.3f);
    Tick::SimulationTickContext next = w.getTickContext(0, 1);
    if (next.tick_index != w.getSimulationTick())
        return fail(name, "tick context does not continue from the simulation tick");
    if (!approx(static_cast<float>(next.world_time), static_cast<float>(w.getWorldTime()), 0.0001f))
        return fail(name, "tick context world time does not match the simulation clock");

    return pass(name);
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

static bool testWishMoveRightMatchesViewRight()
{
    const std::string name = "wish move right matches view right";
    CharacterMoveInput input;
    input.move_right = 1.0f;
    const auto wish = CharacterController::buildWishMove(input, 10.0f);
    if (!approx(wish.direction.x, -1.0f, 0.02f) || !approx(wish.direction.z, 0.0f, 0.02f))
        return fail(name, "positive move_right did not map to engine view-right (-X at yaw 0)");
    CharacterMoveInput left;
    left.move_right = -1.0f;
    const auto wish_left = CharacterController::buildWishMove(left, 10.0f);
    if (!approx(wish_left.direction.x, 1.0f, 0.02f))
        return fail(name, "negative move_right did not map to engine view-left (+X at yaw 0)");
    return pass(name);
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

static float runJumpApex(bool hold_jump)
{
    const auto tuning = makeSourceMovementTestTuning();
    CharacterControllerState state;
    state.grounded = true;
    CharacterMoveInput jump_input;
    jump_input.buttons = CharacterMoveFlags::Jump;
    state = CharacterController::simulateSourceMovement(jump_input, state, tuning);
    float apex = state.position.y;
    for (int i = 0; i < 240; ++i)
    {
        CharacterMoveInput input;
        if (hold_jump)
            input.buttons = CharacterMoveFlags::Jump;
        state = CharacterController::simulateSourceMovement(input, state, tuning);
        state.grounded = false;
        apex = std::max(apex, state.position.y);
        if (state.velocity.y < 0.0f && i > 5)
        {
            // Continue a few ticks past apex for completeness.
        }
    }
    return apex;
}

static bool testJumpApexAndSetVelocity()
{
    const std::string name = "jump sets velocity and apex in range";
    const auto tuning = makeSourceMovementTestTuning();
    CharacterControllerState incoming;
    incoming.grounded = true;
    incoming.velocity = glm::vec3(0.0f, 2.0f, 0.0f);
    CharacterMoveInput jump_input;
    jump_input.buttons = CharacterMoveFlags::Jump;
    const CharacterControllerState jump_tick =
        CharacterController::simulateSourceMovement(jump_input, incoming, tuning);
    if (!approx(jump_tick.velocity.y, tuning.jump_velocity, 0.02f))
        return fail(name, "jump did not set vy to jump_velocity");
    if (!jump_tick.jumped)
        return fail(name, "jumped flag not set");
    const float apex = runJumpApex(true);
    if (apex < 1.0f || apex > 1.45f)
        return fail(name, "jump apex out of [1.0,1.45]m");
    return pass(name);
}

static bool testFrictionReachesExactlyZero()
{
    const std::string name = "friction reaches exactly zero";
    const auto tuning = makeSourceMovementTestTuning();
    CharacterControllerState state;
    state.grounded = true;
    state.velocity = glm::vec3(5.0f, 0.0f, 0.0f);
    for (int i = 0; i < 60; ++i)
    {
        state = CharacterController::simulateSourceMovement(CharacterMoveInput{}, state, tuning);
        state.grounded = true;
    }
    if (!approx(horizontalSpeed(state.velocity), 0.0f, 0.0001f))
        return fail(name, "friction did not reach exactly zero");
    if (std::abs(state.position.x) >= 1.0f)
        return fail(name, "stop distance >= 1.0m");
    return pass(name);
}

static bool testAirAccelCappedFirstTick()
{
    const std::string name = "air accel capped first tick";
    const auto tuning = makeSourceMovementTestTuning();
    CharacterControllerState state;
    state.grounded = false;
    state.was_grounded = false;
    CharacterMoveInput input;
    input.move_forward = 1.0f;
    const CharacterControllerState result =
        CharacterController::simulateSourceMovement(input, state, tuning);
    if (horizontalSpeed(result.velocity) > 0.19f + 0.001f)
        return fail(name, "air accel first tick exceeded capped formula");
    if (horizontalSpeed(result.velocity) <= 0.0f)
        return fail(name, "air accel did not move");
    return pass(name);
}

static bool testSprintExceedsWalkMax()
{
    const std::string name = "sprint exceeds walk max";
    const auto tuning = makeSourceMovementTestTuning();
    CharacterControllerState state;
    state.grounded = true;
    CharacterMoveInput input;
    input.move_forward = 1.0f;
    input.buttons = CharacterMoveFlags::Sprint;
    for (int i = 0; i < 120; ++i)
    {
        state = CharacterController::simulateSourceMovement(input, state, tuning);
        state.grounded = true;
    }
    if (horizontalSpeed(state.velocity) <= tuning.max_speed)
        return fail(name, "sprint did not exceed walk max");
    return pass(name);
}

static bool testCoyoteJump()
{
    const std::string name = "coyote jump fires after leaving ground";
    const auto tuning = makeSourceMovementTestTuning();
    CharacterControllerState state;
    state.grounded = true;
    state = CharacterController::simulateSourceMovement(CharacterMoveInput{}, state, tuning);
    state.grounded = false;
    state = CharacterController::simulateSourceMovement(CharacterMoveInput{}, state, tuning);
    state.grounded = false;
    state = CharacterController::simulateSourceMovement(CharacterMoveInput{}, state, tuning);
    state.grounded = false;
    CharacterMoveInput jump_input;
    jump_input.buttons = CharacterMoveFlags::Jump;
    const CharacterControllerState result =
        CharacterController::simulateSourceMovement(jump_input, state, tuning);
    if (!result.jumped)
        return fail(name, "coyote jump did not fire");
    if (!approx(result.velocity.y, tuning.jump_velocity, 0.02f))
        return fail(name, "coyote jump velocity wrong");
    return pass(name);
}

static bool testJumpBufferFiresOnLanding()
{
    const std::string name = "jump buffer fires on landing";
    const auto tuning = makeSourceMovementTestTuning();
    CharacterControllerState state;
    state.grounded = false;
    state.was_grounded = false;
    state.coyote_timer = 0.0f;
    CharacterMoveInput jump_input;
    jump_input.buttons = CharacterMoveFlags::Jump;
    state = CharacterController::simulateSourceMovement(jump_input, state, tuning);
    if (state.jumped)
        return fail(name, "buffered jump fired in air");
    state.grounded = true;
    const CharacterControllerState result =
        CharacterController::simulateSourceMovement(CharacterMoveInput{}, state, tuning);
    if (!result.jumped)
        return fail(name, "jump buffer did not fire on landing");
    if (!approx(result.velocity.y, tuning.jump_velocity, 0.02f))
        return fail(name, "buffered jump velocity wrong");
    return pass(name);
}

static bool testVariableJumpCutLowersApex()
{
    const std::string name = "variable jump cut lowers apex";
    const float held = runJumpApex(true);
    const float cut = runJumpApex(false);
    if (!(cut < held - 0.2f))
        return fail(name, "jump cut did not lower apex");
    return pass(name);
}

static bool testFallSpeedClampPreservesHorizontal()
{
    const std::string name = "fall speed clamped horizontal preserved";
    const auto tuning = makeSourceMovementTestTuning();
    CharacterControllerState state;
    state.grounded = false;
    state.was_grounded = false;
    state.velocity = glm::vec3(3.0f, -100.0f, 0.0f);
    const CharacterControllerState result =
        CharacterController::simulateSourceMovement(CharacterMoveInput{}, state, tuning);
    if (!approx(result.velocity.y, -tuning.max_fall_speed, 0.02f))
        return fail(name, "fall speed not clamped at max_fall_speed");
    if (!approx(result.velocity.x, 3.0f, 0.02f))
        return fail(name, "horizontal velocity not preserved while falling");
    return pass(name);
}

static bool testSlopeProjectedWish()
{
    const std::string name = "slope projected wish";
    const auto tuning = makeSourceMovementTestTuning();
    CharacterControllerState state;
    state.grounded = true;
    const float s = 0.70710678f;
    state.ground_normal = glm::vec3(s, s, 0.0f);
    CharacterMoveInput input;
    input.move_right = 1.0f;
    const CharacterControllerState result =
        CharacterController::simulateSourceMovement(input, state, tuning);
    const float expected = tuning.ground_acceleration * tuning.max_speed * tuning.fixed_delta * 0.5f;
    if (!approx(result.velocity.x, -expected, 0.03f)) // view-right is -X (see strafe test)
        return fail(name, "slope projection did not scale wish acceleration");
    if (horizontalSpeed(result.velocity) >= tuning.ground_acceleration * tuning.max_speed * tuning.fixed_delta)
        return fail(name, "slope wish retained full flat-ground acceleration");
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

static bool testCharacterControllerPhysicalStepsAndSlopes()
{
    const std::string name = "CharacterController physical steps and slopes";

    auto add_static_box = [](world& w,
                             const glm::vec3& position,
                             const glm::vec3& half_extents,
                             const glm::vec3& rotation)
    {
        const entt::entity entity = w.registry.create();
        w.registry.emplace<TransformComponent>(entity, position.x, position.y, position.z);
        w.registry.emplace<RigidBodyComponent>(entity);
        auto& collider = w.registry.emplace<ColliderComponent>(entity);
        collider.shape_type = ColliderShapeType::Box;
        collider.box_half_extents = half_extents;
        const auto shape = PhysicsSystem::createShapeFromCollider(collider, glm::vec3(1.0f));
        return shape && !w.getPhysicsSystem().createStaticBody(position, rotation, shape, entity).IsInvalid();
    };

    auto make_sharp_step_mesh = [](float step_height)
    {
        std::vector<vertex> vertices;
        vertices.reserve(36);
        auto append_triangle = [&vertices](const glm::vec3& a,
                                           const glm::vec3& b,
                                           const glm::vec3& c)
        {
            auto append_vertex = [&vertices](const glm::vec3& p)
            {
                vertices.push_back(vertex{
                    p.x, p.y, p.z,
                    0.0f, 0.0f, 0.0f,
                    0.0f, 0.0f,
                    0.0f, 0.0f, 0.0f, 0.0f});
            };
            append_vertex(a);
            append_vertex(b);
            append_vertex(c);
        };

        constexpr float half_x = 2.0f;
        constexpr float half_z = 0.5f;
        const float half_y = step_height * 0.5f;
        const glm::vec3 lbn(-half_x, -half_y, -half_z);
        const glm::vec3 ltn(-half_x,  half_y, -half_z);
        const glm::vec3 rtn( half_x,  half_y, -half_z);
        const glm::vec3 rbn( half_x, -half_y, -half_z);
        const glm::vec3 lbp(-half_x, -half_y,  half_z);
        const glm::vec3 ltp(-half_x,  half_y,  half_z);
        const glm::vec3 rtp( half_x,  half_y,  half_z);
        const glm::vec3 rbp( half_x, -half_y,  half_z);

        // Outward-wound triangles retain the box's sharp edges in the static mesh.
        append_triangle(ltn, ltp, rtp); append_triangle(ltn, rtp, rtn); // top (+Y)
        append_triangle(lbn, rbn, rbp); append_triangle(lbn, rbp, lbp); // bottom (-Y)
        append_triangle(lbn, ltn, rtn); append_triangle(lbn, rtn, rbn); // front (-Z)
        append_triangle(lbp, rbp, rtp); append_triangle(lbp, rtp, ltp); // back (+Z)
        append_triangle(lbn, lbp, ltp); append_triangle(lbn, ltp, ltn); // left (-X)
        append_triangle(rbn, rtn, rtp); append_triangle(rbn, rtp, rbp); // right (+X)
        return vertices;
    };

    auto run_step_case = [&](float step_height, bool should_climb)
    {
        world w;
        CharacterPhysicsService character_physics(w);
        w.initializePhysics();
        if (!add_static_box(w, glm::vec3(0.0f, -0.25f, 0.0f),
                            glm::vec3(4.0f, 0.25f, 8.0f), glm::vec3(0.0f)))
            return fail(name, "failed to create step-test floor");

        // Use the public static-mesh path for a sharp ledge. Garden's public
        // box collider intentionally uses Jolt's default convex radius, which
        // rounds the edge and changes the effective step height.
        const entt::entity step_entity = w.registry.create();
        w.registry.emplace<TransformComponent>(step_entity, 0.0f, step_height * 0.5f, 2.0f);
        w.registry.emplace<RigidBodyComponent>(step_entity);
        std::vector<vertex> step_vertices = make_sharp_step_mesh(step_height);
        mesh step_mesh(step_vertices.data(), step_vertices.size());
        if (w.getPhysicsSystem().createStaticMeshBody(
                glm::vec3(0.0f, step_height * 0.5f, 2.0f), glm::vec3(0.0f),
                glm::vec3(1.0f), step_mesh, step_entity).IsInvalid())
            return fail(name, "failed to create step obstacle");

        const entt::entity character = w.registry.create();
        auto& transform = w.registry.emplace<TransformComponent>(character, 0.0f, 0.0f, 0.0f);
        auto& controller = w.registry.emplace<CharacterControllerComponent>(character);
        // Start exactly at floor contact; the previous 5 cm gap combined with
        // collision tolerances near the 5 cm box bevel affected the 0.5 m case.
        transform.position.y = controller.capsule_half_height + controller.capsule_radius;
        controller.move_speed = 4.0f;
        if (w.getPhysicsSystem().createCharacterController(w.registry, character).IsInvalid())
            return fail(name, "failed to create step-test CharacterController");

        CharacterMoveInput forward;
        forward.move_forward = 1.0f;
        bool crossed_step = false;
        float highest_center_on_step = 0.0f;
        float furthest_center = 0.0f;
        for (int tick = 0; tick < 300; ++tick)
        {
            CharacterControllerState state;
            if (!character_physics.simulateController(PhysicsTestEcsBridge::entityRef(w, character), forward, w.fixed_delta, state))
                return fail(name, "failed to simulate step case");
            w.stepPhysicsSingleStep();
            furthest_center = (std::max)(furthest_center, state.position.z);
            if (state.position.z >= 1.4f && state.position.z <= 2.8f)
                highest_center_on_step = (std::max)(highest_center_on_step, state.position.y);
            if (state.position.z > 2.8f)
            {
                crossed_step = true;
                break;
            }
        }

        if (should_climb)
        {
            if (!crossed_step)
                return fail(name, "capsule did not traverse the 0.35 m step");
            if (highest_center_on_step < 1.4f)
                return fail(name, "capsule traversed the 0.35 m step without rising onto its top");
        }
        else
        {
            if (crossed_step || furthest_center >= 1.45f)
                return fail(name, "capsule crossed the 0.5 m step despite the 0.4 m step-up limit");
            if (highest_center_on_step >= 1.4f)
                return fail(name, "capsule rose onto the 0.5 m step");
        }
        return true;
    };

    if (!run_step_case(0.35f, true) || !run_step_case(0.5f, false))
        return false;

    auto run_slope_case = [&](float angle_degrees, bool should_ground)
    {
        world w;
        CharacterPhysicsService character_physics(w);
        w.initializePhysics();
        if (!add_static_box(w, glm::vec3(0.0f, -0.25f, 0.0f),
                            glm::vec3(4.0f, 0.25f, 8.0f), glm::vec3(0.0f)))
            return fail(name, "failed to create slope-test floor");

        constexpr float half_length = 2.5f;
        constexpr float half_thickness = 0.1f;
        const float angle_radians = glm::radians(angle_degrees);
        const float center_y = half_length * std::sin(angle_radians) -
                               half_thickness * std::cos(angle_radians);
        if (!add_static_box(w,
                            glm::vec3(0.0f, center_y, 0.0f),
                            glm::vec3(3.0f, half_thickness, half_length),
                            glm::vec3(-angle_degrees, 0.0f, 0.0f)))
            return fail(name, "failed to create rotated slope collider");

        const float cos_angle = std::cos(angle_radians);
        const float sin_angle = std::sin(angle_radians);
        constexpr float local_surface_z = -1.5f;
        // Transform a point on the box's actual top face (local y =
        // half_thickness) through its X rotation of -angle.
        const float surface_y = center_y + half_thickness * cos_angle +
                                local_surface_z * sin_angle;
        const float start_z = -half_thickness * sin_angle +
                              local_surface_z * cos_angle;
        const entt::entity character = w.registry.create();
        auto& controller = w.registry.emplace<CharacterControllerComponent>(character);
        // CharacterVirtual is centered on the transform. Its vertical support
        // over a slope is half-height*cos(angle) + radius along the slope
        // normal, equivalent to this vertical offset at fixed world z.
        const float capsule_support_height = controller.capsule_half_height +
                                            controller.capsule_radius / cos_angle;
        w.registry.emplace<TransformComponent>(character, 0.0f,
            surface_y + capsule_support_height + 0.02f, start_z);
        if (w.getPhysicsSystem().createCharacterController(w.registry, character).IsInvalid())
            return fail(name, "failed to create slope-test CharacterController");

        bool observed_expected_ground_state = false;
        bool component_state_synced = false;
        CharacterControllerState state;
        for (int tick = 0; tick < 8; ++tick)
        {
            if (!character_physics.simulateController(PhysicsTestEcsBridge::entityRef(w, character), CharacterMoveInput{}, w.fixed_delta, state))
                return fail(name, "failed to simulate slope case");
            w.stepPhysicsSingleStep();
            if (!character_physics.refreshController(PhysicsTestEcsBridge::entityRef(w, character)))
                return fail(name, "failed to refresh slope contacts");
            if (should_ground)
            {
                if (state.grounded && !state.on_steep_ground && state.ground_normal.y > 0.65f)
                    observed_expected_ground_state = true;
            }
            else if (state.on_steep_ground && !state.grounded)
            {
                observed_expected_ground_state = true;
                component_state_synced = controller.on_steep_ground;
            }
        }

        if (!observed_expected_ground_state)
        {
            return fail(name, should_ground
                ? "capsule did not ground on the 40 degree slope"
                : "capsule did not report sliding on steep ground at 60 degrees");
        }
        if (!should_ground && !component_state_synced)
            return fail(name, "on_steep_ground was not synchronized to CharacterControllerComponent");
        return true;
    };

    if (!run_slope_case(40.0f, true) || !run_slope_case(60.0f, false))
        return false;

    return pass(name);
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
    CharacterPhysicsService character_physics(w);
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
    CharacterControllerState result;
    if (!character_physics.simulateController(PhysicsTestEcsBridge::entityRef(w, player), input, w.fixed_delta, result))
        return fail(name, "failed to simulate character in water");

    if (result.water_level < CharacterWaterLevel::Waist)
        return fail(name, "character state did not enter swimming water level");
    const auto& controller = w.registry.get<CharacterControllerComponent>(player);
    if (!controller.swimming)
        return fail(name, "CharacterControllerComponent swimming state was not synced");

    water.affects_swimming = false;
    CharacterControllerState disabled_result;
    if (!character_physics.simulateController(PhysicsTestEcsBridge::entityRef(w, player), input, w.fixed_delta, disabled_result))
        return fail(name, "failed to simulate character in water");
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
    controller.setPossessedPlayer(PhysicsTestEcsBridge::entityRef(w, player));
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

static bool testPlayerControllerUsesLocalEcsInput()
{
    const std::string name = "player controller consumes local ECS input";
    world w;
    auto input = std::make_shared<InputManager>();
    input->update();
    SDL_Event key_down{};
    key_down.type = SDL_EVENT_KEY_DOWN;
    key_down.key.scancode = SDL_SCANCODE_W;
    input->process_event(key_down);

    const entt::entity pawn = w.registry.create();
    w.registry.emplace<TransformComponent>(pawn);
    auto& local_input = w.registry.emplace<LocalPlayerInputComponent>(pawn);
    const entt::entity freecam = w.registry.create();
    w.registry.emplace<TransformComponent>(freecam);
    w.registry.emplace<FreecamComponent>(freecam);

    PlayerController controller(input, &w);
    controller.setPossessedPlayer(PhysicsTestEcsBridge::entityRef(w, pawn));
    controller.setPossessedFreecam(PhysicsTestEcsBridge::entityRef(w, freecam));
    controller.toggleFreecamMode();

    const glm::vec3 initial_position = w.world_camera.position;
    controller.update(0.25f);
    if (glm::distance(w.world_camera.position, initial_position) > 0.001f)
        return fail(name, "held raw W moved freecam without ECS input intent");

    local_input.freecam_move.z = 1.0f;
    controller.update(0.25f);
    const glm::vec3 intent_position = w.world_camera.position;
    if (glm::distance(intent_position, initial_position) < 0.1f)
        return fail(name, "freecam did not consume movement intent from its pawn component");

    local_input.clearIntent();
    controller.update(0.25f);
    if (glm::distance(w.world_camera.position, intent_position) > 0.001f)
        return fail(name, "held raw W moved freecam after ECS intent was cleared");

    auto& freecam_input = w.registry.get<FreecamComponent>(freecam);
    local_input.freecam_move.z = 1.0f;
    freecam_input.input_enabled = false;
    controller.update(0.25f);
    if (glm::distance(w.world_camera.position, intent_position) > 0.001f)
        return fail(name, "disabled freecam input gate did not suppress ECS intent");

    freecam_input.input_enabled = true;
    local_input.input_enabled = false;
    controller.update(0.25f);
    if (glm::distance(w.world_camera.position, intent_position) > 0.001f)
        return fail(name, "disabled local input gate did not suppress ECS intent");

    return pass(name);
}

static bool testPlayerControllerConsumesGamepadLookIntent()
{
    const std::string name = "player controller consumes ECS gamepad look intent";
    world w;
    auto input = std::make_shared<InputManager>();
    input->set_mouse_sensitivity_xy(3.0f, 3.0f);

    const entt::entity pawn = w.registry.create();
    w.registry.emplace<TransformComponent>(pawn);
    auto& player = w.registry.emplace<PlayerComponent>(pawn);
    player.mouse_sensitivity = 2.0f;
    auto& character = w.registry.emplace<CharacterControllerComponent>(pawn);
    auto& local_input = w.registry.emplace<LocalPlayerInputComponent>(pawn);
    local_input.look = glm::vec2(1.0f, 1.0f);

    PlayerController controller(input, &w);
    controller.setPossessedPlayer(PhysicsTestEcsBridge::entityRef(w, pawn));
    controller.setMovementEnabled(false);

    player.input_enabled = false;
    controller.update(0.5f);
    if (glm::length(w.world_camera.rotation) > 0.001f)
        return fail(name, "disabled PlayerComponent input gate allowed camera look");

    player.input_enabled = true;
    character.input_enabled = false;
    controller.update(0.5f);
    if (glm::length(w.world_camera.rotation) > 0.001f)
        return fail(name, "disabled CharacterController input gate allowed camera look");

    character.input_enabled = true;
    local_input.input_enabled = false;
    controller.update(0.5f);
    if (glm::length(w.world_camera.rotation) > 0.001f)
        return fail(name, "disabled transient local input gate allowed camera look");

    local_input.input_enabled = true;
    for (int frame = 0; frame < 30; ++frame)
        controller.update(1.0f / 60.0f);
    const glm::vec3 thirty_frame_rotation = w.world_camera.rotation;
    if (!approx(thirty_frame_rotation.y, -3.0f, 0.01f) ||
        !approx(thirty_frame_rotation.x, 1.5f, 0.01f))
        return fail(name, "look sensitivity or pitch clamp was not applied per player");

    w.world_camera.rotation = glm::vec3(0.0f);
    controller.updatePlayerLook(0.5f);
    controller.update(0.5f);
    if (!approx(w.world_camera.rotation.y, thirty_frame_rotation.y, 0.01f) ||
        !approx(w.world_camera.rotation.x, thirty_frame_rotation.x, 0.01f))
        return fail(name, "host-applied look intent was consumed twice by controller update");

    w.world_camera.rotation = glm::vec3(0.0f);
    controller.update(0.5f);
    if (!approx(w.world_camera.rotation.y, thirty_frame_rotation.y, 0.01f) ||
        !approx(w.world_camera.rotation.x, thirty_frame_rotation.x, 0.01f))
        return fail(name, "gamepad look changed with frame rate for equal elapsed time");

    w.world_camera.rotation = glm::vec3(0.0f);
    controller.handleMouseMotion(10.0f, 10.0f);
    if (!approx(w.world_camera.rotation.y, -0.06f, 0.001f) ||
        !approx(w.world_camera.rotation.x, 0.06f, 0.001f))
        return fail(name, "existing mouse-look sensitivity behavior changed");

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
    CharacterPhysicsService character_physics(w);
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
        CharacterControllerState state;
        if (!character_physics.simulateController(PhysicsTestEcsBridge::entityRef(w, player), input, w.fixed_delta, state))
            return fail(name, "failed to simulate character");
        w.getPhysicsSystem().stepPhysics(w.registry);
        if (!character_physics.refreshController(PhysicsTestEcsBridge::entityRef(w, player)))
            return fail(name, "failed to refresh character contacts");
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

static bool testD6CookedMeshAndWaterMaterialExclusion()
{
    const std::string name = "D6-T6 cooked mesh raycast, character ground, and water exclusion";

    const auto make_vertex = [](float x, float z)
    {
        return vertex{x, 0.0f, z, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    };

    // Two separate horizontal quads: a solid region at x=[-3,-1] and a water
    // region at x=[1,3]. Both are written into the same cooked cache.
    std::vector<vertex> source_vertices = {
        make_vertex(-3.0f, -2.0f), make_vertex(-3.0f,  2.0f), make_vertex(-1.0f,  2.0f),
        make_vertex(-3.0f, -2.0f), make_vertex(-1.0f,  2.0f), make_vertex(-1.0f, -2.0f),
        make_vertex( 1.0f, -2.0f), make_vertex( 1.0f,  2.0f), make_vertex( 3.0f,  2.0f),
        make_vertex( 1.0f, -2.0f), make_vertex( 3.0f,  2.0f), make_vertex( 3.0f, -2.0f),
    };

    const fs::path cache_directory = fs::temp_directory_path() /
        ("garden_physics_d6t6_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code create_error;
    if (!fs::create_directory(cache_directory, create_error))
        return fail(name, "failed to create isolated temporary cache directory: " + create_error.message());

    struct ScopedDirectoryCleanup
    {
        fs::path path;
        ~ScopedDirectoryCleanup()
        {
            std::error_code error;
            fs::remove_all(path, error);
        }
    } cleanup{cache_directory};

    const fs::path cooked_path = cache_directory / "d6_t6_mesh.gardenphys";
    Assets::LODMeshData cooked_data;
    cooked_data.vertices = source_vertices;
    constexpr uint64_t source_hash = 0xD6D6D6D6D6D6D6D6ull;
    const uint64_t source_file_size = static_cast<uint64_t>(source_vertices.size() * sizeof(vertex));
    std::string cache_error;
    if (!Assets::CookedCollisionSerializer::cookMeshToFile(
            cooked_data, cooked_path.string(), source_hash, source_file_size, nullptr, &cache_error))
        return fail(name, "failed to generate cooked mesh fixture: " + cache_error);

    JPH::ShapeRefC loaded_shape;
    if (!Assets::CookedCollisionSerializer::loadShape(
            cooked_path.string(), source_hash, source_file_size, loaded_shape, &cache_error) || !loaded_shape)
        return fail(name, "generated cooked mesh fixture could not be read: " + cache_error);

    world w;
    CharacterPhysicsService character_physics(w);
    w.initializePhysics();
    PhysicsSystem& physics = w.getPhysicsSystem();

    // This cache-only mesh has no source triangles to fall back to, so a
    // successful body and raycast prove the runtime path restored the cache.
    mesh cooked_only_mesh(nullptr, 0);
    cooked_only_mesh.collision_cache_path = cooked_path.string();
    cooked_only_mesh.collision_source_hash = source_hash;
    cooked_only_mesh.collision_source_file_size = source_file_size;
    const entt::entity cooked_ground = w.registry.create();
    if (physics.createStaticMeshBody(glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(1.0f),
                                     cooked_only_mesh, cooked_ground).IsInvalid())
        return fail(name, "cache-only static mesh body could not be created");

    const auto cooked_hit = physics.raycastClosest(
        glm::vec3(-2.0f, 5.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 10.0f);
    if (!cooked_hit.hit || cooked_hit.entity != cooked_ground || cooked_hit.hit_normal.y < 0.8f)
        return fail(name, "raycast did not hit the generated/read cooked static mesh");

    const auto cooked_water_region_hit = physics.raycastClosest(
        glm::vec3(2.0f, 5.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 10.0f);
    if (!cooked_water_region_hit.hit || cooked_water_region_hit.entity != cooked_ground)
        return fail(name, "cooked cache did not restore the second mesh region");

    const entt::entity character = w.registry.create();
    w.registry.emplace<TransformComponent>(character, -2.0f, 3.0f, 0.0f);
    w.registry.emplace<CharacterControllerComponent>(character);
    if (physics.createCharacterController(w.registry, character).IsInvalid())
        return fail(name, "CharacterController could not be created above the cooked solid region");

    for (int tick = 0; tick < 90; ++tick)
    {
        CharacterControllerState state;
        if (!character_physics.simulateController(PhysicsTestEcsBridge::entityRef(w, character), CharacterMoveInput{}, w.fixed_delta, state))
            return fail(name, "failed to simulate character over cooked mesh");
        physics.stepPhysics(w.registry);
    }
    CharacterControllerState character_state;
    if (!character_physics.readControllerState(PhysicsTestEcsBridge::entityRef(w, character), character_state))
        return fail(name, "failed to read character state over cooked mesh");
    if (!character_state.grounded || character_state.ground_normal.y < 0.8f)
        return fail(name, "CharacterController did not ground on the cooked solid mesh");

    // The source mesh matches the cache data, and the valid cache contains
    // both regions. Water ranges must bypass that cache and filter their
    // triangles while the solid range remains collidable.
    mesh water_source_mesh(source_vertices.data(), source_vertices.size());
    water_source_mesh.uses_material_ranges = true;
    water_source_mesh.material_ranges.emplace_back(0, 6, INVALID_TEXTURE, "stone");
    MaterialRange water_range(6, 6, INVALID_TEXTURE, "ocean");
    water_range.material_flags |= MaterialFlags::Water;
    water_source_mesh.material_ranges.push_back(water_range);
    water_source_mesh.collision_cache_path = cooked_path.string();
    water_source_mesh.collision_source_hash = source_hash;
    water_source_mesh.collision_source_file_size = source_file_size;

    const entt::entity water_test_ground = w.registry.create();
    if (physics.createStaticMeshBody(glm::vec3(10.0f, 0.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f),
                                     water_source_mesh, water_test_ground).IsInvalid())
        return fail(name, "water-range source mesh body could not be created");

    const auto solid_hit = physics.raycastClosest(
        glm::vec3(8.0f, 5.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 10.0f);
    if (!solid_hit.hit || solid_hit.entity != water_test_ground)
        return fail(name, "solid material region was absent from the water-range mesh body");

    const auto water_hit = physics.raycastClosest(
        glm::vec3(12.0f, 5.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 10.0f);
    if (water_hit.hit)
        return fail(name, "water material triangles remained collidable despite the material range");

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


// ---- D6 Stage-2 tests ----
static int g_trigger_entered = 0;
static int g_trigger_exited = 0;
static uint32_t g_last_trigger_id = 0;
static void onTriggerEntered(const TriggerEnteredEvent& e) { ++g_trigger_entered; g_last_trigger_id = e.trigger_id; }
static void onTriggerExited(const TriggerExitedEvent& e) { ++g_trigger_exited; g_last_trigger_id = e.trigger_id; }

struct TriggerLifecycleCapture
{
    std::vector<TriggerEnteredEvent> entered;
    std::vector<TriggerExitedEvent> exited;
};

static void captureTriggerEntered(TriggerLifecycleCapture& capture, const TriggerEnteredEvent& event)
{
    capture.entered.push_back(event);
}

static void captureTriggerExited(TriggerLifecycleCapture& capture, const TriggerExitedEvent& event)
{
    capture.exited.push_back(event);
}

static entt::entity makeDynamicBox(world& w, const glm::vec3& pos, bool gravity = false)
{
    auto e = w.registry.create();
    w.registry.emplace<TransformComponent>(e, pos.x, pos.y, pos.z);
    auto& rb = w.registry.emplace<RigidBodyComponent>(e);
    rb.mass = 1.0f; rb.apply_gravity = gravity;
    PhysicsSystem::PhysicsBodyDesc desc; desc.mass = 1.0f; desc.apply_gravity = gravity;
    w.getPhysicsSystem().createDynamicBody(pos, glm::vec3(0.0f), makeBoxShape(), e, desc);
    return e;
}
static entt::entity makeStaticBox(world& w, const glm::vec3& pos)
{
    auto e = w.registry.create();
    w.registry.emplace<TransformComponent>(e, pos.x, pos.y, pos.z);
    w.registry.emplace<RigidBodyComponent>(e);
    w.getPhysicsSystem().createStaticBody(pos, glm::vec3(0.0f), makeBoxShape(), e);
    return e;
}
static entt::entity makeTriggerBox(world& w, const glm::vec3& pos, float half, uint32_t id)
{
    auto e = w.registry.create();
    w.registry.emplace<TransformComponent>(e, pos.x, pos.y, pos.z);
    auto& col = w.registry.emplace<ColliderComponent>(e);
    col.shape_type = ColliderShapeType::Box; col.box_half_extents = glm::vec3(half);
    auto& trig = w.registry.emplace<TriggerComponent>(e);
    trig.trigger_id = id; trig.enabled = true;
    w.getPhysicsSystem().createTriggerBody(w.registry, e);
    return e;
}

static bool testD6LayerMatrix()
{
    const std::string name = "D6 layer matrix";
    if (PhysicsObjectLayers::StaticWorld != 0 || PhysicsObjectLayers::DynamicProp != 1 ||
        PhysicsObjectLayers::Character != 2 || PhysicsObjectLayers::Trigger != 3 ||
        PhysicsObjectLayers::ProjectileQueryOnly != 4 || PhysicsObjectLayers::Count != 5)
        return fail(name, "layer numbering is not 0..4 as decided");
    PhysicsLayerSettings s;
    auto C = [&](JPH::ObjectLayer a, JPH::ObjectLayer b) { return s.shouldObjectsCollide(a, b); };
    using namespace PhysicsObjectLayers;
    if (!C(StaticWorld, DynamicProp) || !C(DynamicProp, StaticWorld)) return fail(name, "static-dynamic must collide");
    if (!C(StaticWorld, Character) || !C(Character, StaticWorld)) return fail(name, "static-character must collide");
    if (!C(DynamicProp, Character) || !C(Character, DynamicProp)) return fail(name, "dynamic-character must collide");
    if (!C(DynamicProp, DynamicProp) || !C(Character, Character)) return fail(name, "dynamic/character self must collide");
    if (C(StaticWorld, StaticWorld)) return fail(name, "static-static must not collide");
    const JPH::ObjectLayer all[] = {StaticWorld, DynamicProp, Character, Trigger, ProjectileQueryOnly};
    for (auto a : all) for (auto b : all)
    {
        if (a == Trigger || b == Trigger) { if (C(a, b)) return fail(name, "trigger must collide with nothing"); }
        if (a == ProjectileQueryOnly || b == ProjectileQueryOnly) { if (C(a, b)) return fail(name, "projectile-query-only must collide with nothing"); }
        if (C(a, b) != C(b, a)) return fail(name, "pair matrix must be symmetric");
    }
    // Physical: value 0/1 behaviour preserved - dynamic rests on static, trigger does not support it.
    world w; w.initializePhysics(); w.setGravity(glm::vec3(0.0f, -1.0f, 0.0f));
    makeStaticBox(w, glm::vec3(0.0f, 0.0f, 0.0f));
    auto crate = makeDynamicBox(w, glm::vec3(0.0f, 2.0f, 0.0f), true);
    auto floater = makeDynamicBox(w, glm::vec3(10.0f, 2.0f, 0.0f), true);
    makeTriggerBox(w, glm::vec3(10.0f, 0.5f, 0.0f), 2.0f, 1);
    for (int i = 0; i < 120; ++i) w.getPhysicsSystem().stepPhysics(w.registry);
    float crate_y = w.registry.get<TransformComponent>(crate).position.y;
    float floater_y = w.registry.get<TransformComponent>(floater).position.y;
    if (!approx(crate_y, 1.0f, 0.15f)) return fail(name, "dynamic did not rest on static (0/1 behaviour changed)");
    if (floater_y > 0.0f) return fail(name, "trigger volume supported a dynamic body (sensor collided)");
    return pass(name);
}

static bool testD6OverlapAnalyticCases()
{
    const std::string name = "D6 analytic overlap inside edge and outside";
    world w; w.initializePhysics();

    // Each overlap query gets an isolated three-case set of axis-aligned boxes.
    // Their half-extents are 0.5, so the exact touch point is the query's
    // analytic half-extent plus 0.5 along one axis.
    const glm::vec3 sphere_inside_center(0.0f, 0.0f, 0.0f);
    const glm::vec3 sphere_touch_center(10.0f, 0.0f, 0.0f);
    const glm::vec3 sphere_outside_center(20.0f, 0.0f, 0.0f);
    const glm::vec3 box_inside_center(40.0f, 0.0f, 0.0f);
    const glm::vec3 box_touch_center(50.0f, 0.0f, 0.0f);
    const glm::vec3 box_outside_center(60.0f, 0.0f, 0.0f);
    const glm::vec3 capsule_inside_center(80.0f, 0.0f, 0.0f);
    const glm::vec3 capsule_touch_center(90.0f, 0.0f, 0.0f);
    const glm::vec3 capsule_outside_center(100.0f, 0.0f, 0.0f);
    const auto sphere_inside = makeStaticBox(w, sphere_inside_center);
    const auto sphere_touch = makeStaticBox(w, sphere_touch_center + glm::vec3(1.5f, 0.0f, 0.0f));
    makeStaticBox(w, sphere_outside_center + glm::vec3(1.51f, 0.0f, 0.0f));
    const auto box_inside = makeStaticBox(w, box_inside_center);
    const auto box_touch = makeStaticBox(w, box_touch_center + glm::vec3(1.5f, 0.0f, 0.0f));
    makeStaticBox(w, box_outside_center + glm::vec3(1.51f, 0.0f, 0.0f));
    const auto capsule_inside = makeStaticBox(w, capsule_inside_center);
    const auto capsule_touch = makeStaticBox(w, capsule_touch_center + glm::vec3(0.0f, 1.5f, 0.0f));
    makeStaticBox(w, capsule_outside_center + glm::vec3(0.0f, 1.51f, 0.0f));

    PhysicsSystem::QueryFilter f;
    PhysicsSystem::OverlapResult out[8];

    auto expectSingle = [&](const char* query, const char* relation, uint32_t count, entt::entity expected) {
        if (count == 1 && out[0].entity == expected) return true;
        std::cerr << "  " << query << " " << relation << ": expected one entity "
                  << entt::to_integral(expected) << ", got " << count;
        if (count == 1) std::cerr << " (entity " << entt::to_integral(out[0].entity) << ")";
        std::cerr << std::endl;
        return false;
    };
    auto expectNone = [&](const char* query, const char* relation, uint32_t count) {
        if (count == 0) return true;
        std::cerr << "  " << query << " " << relation << ": expected no entities, got " << count << std::endl;
        return false;
    };

    uint32_t n = w.getPhysicsSystem().overlapSphere(sphere_inside_center, 1.0f, f, out, 8);
    if (!expectSingle("sphere", "inside", n, sphere_inside)) return fail(name, "sphere inside case failed");
    n = w.getPhysicsSystem().overlapSphere(sphere_touch_center, 1.0f, f, out, 8);
    // At exact contact, the sphere and box surfaces meet at x = 1.0.
    if (!expectSingle("sphere", "edge touch", n, sphere_touch)) return fail(name, "sphere edge-touch case failed");
    n = w.getPhysicsSystem().overlapSphere(sphere_outside_center, 1.0f, f, out, 8);
    if (!expectNone("sphere", "outside", n)) return fail(name, "sphere outside case failed");

    n = w.getPhysicsSystem().overlapBox(box_inside_center, glm::vec3(1.0f), glm::vec3(0.0f), f, out, 8);
    if (!expectSingle("box", "inside", n, box_inside)) return fail(name, "box inside case failed");
    n = w.getPhysicsSystem().overlapBox(box_touch_center, glm::vec3(1.0f), glm::vec3(0.0f), f, out, 8);
    if (!expectSingle("box", "edge touch", n, box_touch)) return fail(name, "box edge-touch case failed");
    n = w.getPhysicsSystem().overlapBox(box_outside_center, glm::vec3(1.0f), glm::vec3(0.0f), f, out, 8);
    if (!expectNone("box", "outside", n)) return fail(name, "box outside case failed");

    n = w.getPhysicsSystem().overlapCapsule(capsule_inside_center, 0.5f, 0.5f, f, out, 8);
    if (!expectSingle("capsule", "inside", n, capsule_inside)) return fail(name, "capsule inside case failed");
    n = w.getPhysicsSystem().overlapCapsule(capsule_touch_center, 0.5f, 0.5f, f, out, 8);
    if (!expectSingle("capsule", "edge touch", n, capsule_touch)) return fail(name, "capsule edge-touch case failed");
    n = w.getPhysicsSystem().overlapCapsule(capsule_outside_center, 0.5f, 0.5f, f, out, 8);
    if (!expectNone("capsule", "outside", n)) return fail(name, "capsule outside case failed");

    // Preserve coverage for ignore and layer filters with a separate dynamic probe.
    auto filter_probe = makeDynamicBox(w, glm::vec3(200.0f, 0.0f, 0.0f));
    n = w.getPhysicsSystem().overlapSphere(glm::vec3(200.0f, 0.0f, 0.0f), 2.0f, f, out, 8);
    if (!expectSingle("sphere", "filter probe", n, filter_probe))
        return fail(name, "overlap filter probe was not found");
    PhysicsSystem::QueryFilter ignore_f; ignore_f.ignore = filter_probe;
    n = w.getPhysicsSystem().overlapSphere(glm::vec3(200.0f, 0.0f, 0.0f), 2.0f, ignore_f, out, 8);
    if (n != 0) return fail(name, "overlap ignored entity was still reported");
    PhysicsSystem::QueryFilter static_only;
    static_only.layer_mask = PhysicsObjectLayers::layerBit(PhysicsObjectLayers::StaticWorld);
    n = w.getPhysicsSystem().overlapSphere(glm::vec3(200.0f, 0.0f, 0.0f), 2.0f, static_only, out, 8);
    if (n != 0) return fail(name, "overlap layer_mask did not exclude dynamic bodies");
    return pass(name);
}

static bool testD6QueryFilterTriggers()
{
    const std::string name = "D6 query filter excludes triggers by default";
    world w; w.initializePhysics();
    auto trig = makeTriggerBox(w, glm::vec3(0.0f, 0.0f, 3.0f), 0.5f, 42);
    auto wall = makeStaticBox(w, glm::vec3(0.0f, 0.0f, 6.0f));
    auto def = w.getPhysicsSystem().raycastClosest(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), 20.0f);
    if (!def.hit || def.entity != wall) return fail(name, "default raycast did not skip trigger and hit wall");
    PhysicsSystem::QueryFilter with_trig; with_trig.hit_triggers = true;
    auto th = w.getPhysicsSystem().raycastClosest(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), 20.0f, with_trig);
    if (!th.hit || th.entity != trig) return fail(name, "hit_triggers=true raycast did not hit trigger first");
    PhysicsSystem::RaycastResult all[8];
    uint32_t n = w.getPhysicsSystem().raycastAll(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), 20.0f, with_trig, all, 8);
    if (n != 2 || all[0].entity != trig || all[1].entity != wall || all[0].distance > all[1].distance)
        return fail(name, "raycastAll did not return trigger+wall distance-sorted");
    n = w.getPhysicsSystem().raycastAll(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), 20.0f, PhysicsSystem::QueryFilter{}, all, 8);
    if (n != 1 || all[0].entity != wall) return fail(name, "raycastAll default did not exclude trigger");
    // Shape casts honour the filter too.
    auto sc_def = w.getPhysicsSystem().sphereCast(glm::vec3(0.0f, 0.0f, 0.0f), 0.2f, glm::vec3(0.0f, 0.0f, 1.0f), 20.0f, PhysicsSystem::QueryFilter{});
    if (!sc_def.hit || sc_def.entity != wall) return fail(name, "filtered sphereCast did not skip trigger");
    auto sc_trig = w.getPhysicsSystem().sphereCast(glm::vec3(0.0f, 0.0f, 0.0f), 0.2f, glm::vec3(0.0f, 0.0f, 1.0f), 20.0f, with_trig);
    if (!sc_trig.hit || sc_trig.entity != trig) return fail(name, "sphereCast with hit_triggers did not hit trigger");
    return pass(name);
}

static bool testD6QueryIgnoreListRaycasts()
{
    const std::string name = "D6 query ignore-list raycasts";
    world w; w.initializePhysics();

    const auto nearer_body = makeStaticBox(w, glm::vec3(0.0f, 0.0f, 2.0f));
    const auto farther_body = makeStaticBox(w, glm::vec3(0.0f, 0.0f, 5.0f));
    const glm::vec3 origin(0.0f, 0.0f, 0.0f);
    const glm::vec3 direction(0.0f, 0.0f, 1.0f);

    PhysicsSystem::RaycastResult hits[4];
    uint32_t count = w.getPhysicsSystem().raycastAll(
        origin, direction, 10.0f, PhysicsSystem::QueryFilter{}, hits, 4);
    if (count != 2 || hits[0].entity != nearer_body || hits[1].entity != farther_body ||
        hits[0].distance >= hits[1].distance)
        return fail(name, "unfiltered raycastAll did not return both boxes distance-sorted");

    const entt::entity ignored_entities[] = {nearer_body};
    PhysicsSystem::QueryFilter ignore_nearer;
    ignore_nearer.ignore_list = ignored_entities;
    ignore_nearer.ignore_count = 1;

    const auto closest = w.getPhysicsSystem().raycastClosest(origin, direction, 10.0f, ignore_nearer);
    if (!closest.hit || closest.entity != farther_body)
        return fail(name, "raycastClosest did not continue to the farther body after ignoring the nearer one");

    count = w.getPhysicsSystem().raycastAll(origin, direction, 10.0f, ignore_nearer, hits, 4);
    if (count != 1 || hits[0].entity != farther_body)
        return fail(name, "raycastAll did not exclude the ignored body");

    return pass(name);
}

static bool testD6DynamicBodyTeleport()
{
    const std::string name = "D6 dynamic body teleport";
    world w;
    w.initializePhysics();

    auto make_box_shape = [](const glm::vec3& half_extents)
    {
        ColliderComponent collider;
        collider.shape_type = ColliderShapeType::Box;
        collider.box_half_extents = half_extents;
        return PhysicsSystem::createShapeFromCollider(collider, glm::vec3(1.0f));
    };

    const entt::entity floor = w.registry.create();
    w.registry.emplace<TransformComponent>(floor, 0.0f, -0.5f, 0.0f);
    w.registry.emplace<RigidBodyComponent>(floor);
    const auto floor_shape = make_box_shape(glm::vec3(5.0f, 0.5f, 5.0f));
    if (!floor_shape)
        return fail(name, "failed to create resting floor");
    w.getPhysicsSystem().createStaticBody(
        glm::vec3(0.0f, -0.5f, 0.0f), glm::vec3(0.0f), floor_shape, floor);
    if (!w.getPhysicsSystem().hasBody(floor))
        return fail(name, "failed to create resting floor body");

    const entt::entity crate = w.registry.create();
    auto& crate_transform = w.registry.emplace<TransformComponent>(crate, 0.0f, 5.0f, 0.0f);
    auto& crate_body = w.registry.emplace<RigidBodyComponent>(crate);
    crate_body.velocity = glm::vec3(8.0f, 1.0f, -3.0f);
    const auto crate_shape = make_box_shape(glm::vec3(0.5f));
    if (!crate_shape)
        return fail(name, "failed to create dynamic crate");
    w.getPhysicsSystem().createDynamicBody(
        crate_transform.position, crate_transform.rotation, crate_shape, crate);
    if (!w.getPhysicsSystem().hasBody(crate))
        return fail(name, "failed to create dynamic crate body");

    // Seed non-zero Jolt velocity before teleporting so the test checks that
    // the API clears both the ECS value and the live body motion state.
    w.stepPhysicsSingleStep();
    crate_body.force = glm::vec3(200.0f, 0.0f, 0.0f);
    const glm::vec3 target(1.0f, 0.5f, 1.0f);
    const glm::vec3 target_rotation(0.0f, 35.0f, 0.0f);
    if (!w.teleport_body(crate, target, target_rotation))
        return fail(name, "generic world teleport rejected a dynamic body");
    if (glm::distance(crate_transform.position, target) > 0.001f ||
        glm::distance(crate_transform.rotation, target_rotation) > 0.001f)
        return fail(name, "teleport did not update the ECS pose");
    if (glm::length(crate_body.velocity) > 0.001f || glm::length(crate_body.force) > 0.001f)
        return fail(name, "teleport did not clear dynamic velocity and accumulated ECS force");
    w.getPhysicsSystem().syncTransformsFromJolt(w.registry);
    if (glm::length(crate_body.velocity) > 0.001f)
        return fail(name, "teleport left stale velocity on the live Jolt body");

    w.stepPhysicsSingleStep();
    if (std::abs(crate_transform.position.x - target.x) > 0.03f ||
        std::abs(crate_transform.position.z - target.z) > 0.03f)
        return fail(name, "crate retained its pre-teleport horizontal velocity");
    for (int tick = 0; tick < 60; ++tick)
        w.stepPhysicsSingleStep();
    if (std::abs(crate_transform.position.y - target.y) > 0.03f ||
        glm::length(crate_body.velocity) > 0.1f)
        return fail(name, "crate did not settle at the teleported resting target");

    return pass(name);
}

static bool testD6KinematicPlatformCarriesCharacter()
{
    const std::string name = "D6 kinematic platform carries CharacterController";
    world w;
    CharacterPhysicsService character_physics(w);
    w.initializePhysics();

    ColliderComponent platform_collider;
    platform_collider.shape_type = ColliderShapeType::Box;
    platform_collider.box_half_extents = glm::vec3(2.0f, 0.25f, 2.0f);
    const auto platform_shape = PhysicsSystem::createShapeFromCollider(platform_collider, glm::vec3(1.0f));
    const entt::entity platform = w.registry.create();
    auto& platform_transform = w.registry.emplace<TransformComponent>(platform, 0.0f, 0.25f, 0.0f);
    auto& platform_body = w.registry.emplace<RigidBodyComponent>(platform);
    platform_body.motion_type = BodyMotionType::Kinematic;
    platform_body.apply_gravity = false;
    if (!platform_shape)
        return fail(name, "failed to create kinematic platform");
    w.getPhysicsSystem().createKinematicBody(
        platform_transform.position, platform_transform.rotation, platform_shape, platform);
    if (!w.getPhysicsSystem().hasBody(platform))
        return fail(name, "failed to create kinematic platform body");

    const entt::entity character = w.registry.create();
    auto& character_transform = w.registry.emplace<TransformComponent>(character, 0.0f, 1.75f, 0.0f);
    w.registry.emplace<CharacterControllerComponent>(character);
    w.getPhysicsSystem().createCharacterController(w.registry, character);
    if (!w.getPhysicsSystem().hasCharacterController(character))
        return fail(name, "failed to create CharacterController");

    auto step_character = [&]()
    {
        CharacterControllerState state;
        if (!character_physics.simulateController(PhysicsTestEcsBridge::entityRef(w, character), CharacterMoveInput{}, w.fixed_delta, state))
            return false;
        w.stepPhysicsSingleStep();
        return character_physics.refreshController(PhysicsTestEcsBridge::entityRef(w, character));
    };
    for (int tick = 0; tick < 60; ++tick)
        if (!step_character())
            return fail(name, "failed to advance character on platform");
    CharacterControllerState settled_state;
    if (!character_physics.readControllerState(PhysicsTestEcsBridge::entityRef(w, character), settled_state) || !settled_state.grounded)
        return fail(name, "capsule did not settle on the kinematic platform");

    const float start_x = character_transform.position.x;
    for (int tick = 0; tick < 30; ++tick)
    {
        platform_transform.position.x += 0.05f;
        if (!step_character())
            return fail(name, "failed to advance character on platform");
    }

    CharacterControllerState state;
    if (!character_physics.readControllerState(PhysicsTestEcsBridge::entityRef(w, character), state))
        return fail(name, "failed to read carried character state");
    if (!state.grounded || state.ground_velocity.x < 0.1f)
        return fail(name, "moving platform did not report non-zero character ground velocity");
    if (character_transform.position.x < start_x + 0.25f)
        return fail(name, "standing capsule was not carried by the moving platform");

    return pass(name);
}

// H-E portions 2 and 3: a hierarchy-child kinematic body takes its pose from
// the composed *current* local hierarchy, while its TransformComponent remains
// the authored local value.  The character is deliberately left as a root:
// it is an occupant of the parented Jolt platform, never a hierarchy child.
static bool testHEParentedKinematicPlatformSync()
{
    const std::string name = "H-E parented kinematic sync composes pose and carries CharacterVirtual";
    world w;
    CharacterPhysicsService character_physics(w);
    w.initializePhysics();
    EntityLifecycle life(w.registry, 71);

    const entt::entity parent = life.create();
    auto& parent_transform = w.registry.emplace<TransformComponent>(parent, 4.0f, 0.0f, -3.0f);
    parent_transform.rotation = {0.0f, 35.0f, 0.0f};

    const entt::entity platform = life.create();
    auto& platform_transform = w.registry.emplace<TransformComponent>(platform, 1.25f, 0.25f, 0.0f);
    platform_transform.rotation = {0.0f, 20.0f, 0.0f};
    const TransformComponent platform_local_before = platform_transform;
    auto& platform_body = w.registry.emplace<RigidBodyComponent>(platform);
    platform_body.motion_type = BodyMotionType::Kinematic;
    platform_body.apply_gravity = false;
    if (!Hierarchy::attach(w.registry, platform, parent))
        return fail(name, "valid kinematic child attach was rejected");

    ColliderComponent platform_collider;
    platform_collider.shape_type = ColliderShapeType::Box;
    platform_collider.box_half_extents = glm::vec3(2.0f, 0.25f, 2.0f);
    const auto platform_shape = PhysicsSystem::createShapeFromCollider(platform_collider, glm::vec3(1.0f));
    if (!platform_shape)
        return fail(name, "failed to create hierarchy-platform shape");

    const glm::mat4 initial_world = Hierarchy::computeWorldMatrix(w.registry, platform);
    const glm::vec3 initial_position(initial_world[3]);
    const JPH::BodyID platform_body_id = w.getPhysicsSystem().createKinematicBody(
        initial_position, glm::vec3(0.0f), platform_shape, platform);
    if (platform_body_id.IsInvalid())
        return fail(name, "failed to create hierarchy-platform body");

    const entt::entity character = w.registry.create();
    auto& character_transform = w.registry.emplace<TransformComponent>(
        character, initial_position.x, initial_position.y + 1.5f, initial_position.z);
    w.registry.emplace<CharacterControllerComponent>(character);
    if (w.getPhysicsSystem().createCharacterController(w.registry, character).IsInvalid())
        return fail(name, "failed to create CharacterVirtual occupant");

    // Observe the body solely through the engine's public collision-query
    // boundary. The vertical probe is aimed at the expected composed centre;
    // the side probe checks both the expected translated face and its rotated
    // outward normal. Moving the parent six units below ensures that an old
    // body pose cannot satisfy the centre probe by still overlapping it.
    auto pose_matches_query = [&](const glm::mat4& world_pose, const char* stage)
    {
        const glm::vec3 center(world_pose[3]);
        const glm::vec3 axis_x = glm::normalize(glm::vec3(world_pose[0]));
        const glm::vec3 axis_y = glm::normalize(glm::vec3(world_pose[1]));
        PhysicsSystem::QueryFilter filter;
        filter.ignore = character;

        const auto center_hit = w.getPhysicsSystem().raycastClosest(
            center + axis_y * 4.0f, -axis_y, 8.0f, filter);
        if (!center_hit.hit || center_hit.entity != platform)
            return fail(name, std::string(stage) + " kinematic body was absent from its composed world position");

        const auto side_hit = w.getPhysicsSystem().raycastClosest(
            center + axis_x * 4.0f, -axis_x, 8.0f, filter);
        const glm::vec3 expected_side = center + axis_x * platform_collider.box_half_extents.x;
        if (!side_hit.hit || side_hit.entity != platform ||
            glm::distance(side_hit.hit_point, expected_side) > 0.03f)
            return fail(name, std::string(stage) + " kinematic body did not follow parent translation");
        if (glm::dot(glm::normalize(side_hit.hit_normal), axis_x) < 0.999f)
            return fail(name, std::string(stage) + " kinematic body did not compose parent and child rotation");
        return true;
    };

    auto simulate_occupant = [&]()
    {
        CharacterControllerState state;
        if (!character_physics.simulateController(PhysicsTestEcsBridge::entityRef(w, character), CharacterMoveInput{}, w.fixed_delta, state))
            return false;
        w.stepPhysicsSingleStep();
        return character_physics.refreshController(PhysicsTestEcsBridge::entityRef(w, character));
    };
    for (int tick = 0; tick < 60; ++tick)
        if (!simulate_occupant())
            return fail(name, "failed to advance character on platform");
    CharacterControllerState settled_state;
    if (!character_physics.readControllerState(PhysicsTestEcsBridge::entityRef(w, character), settled_state) || !settled_state.grounded)
        return fail(name, "CharacterVirtual did not settle on hierarchy platform");

    const float character_start_x = character_transform.position.x;
    for (int tick = 0; tick < 60; ++tick)
    {
        // Change only the ancestor. The platform local TRS must stay local.
        parent_transform.position.x += 0.10f;
        parent_transform.rotation.y += 1.0f;
        if (!simulate_occupant())
            return fail(name, "failed to advance character on platform");
    }

    const glm::mat4 expected_world = Hierarchy::computeWorldMatrix(w.registry, platform);
    if (!pose_matches_query(expected_world, "parent motion"))
        return false;

    if (glm::distance(platform_transform.position, platform_local_before.position) > 0.0001f ||
        glm::distance(platform_transform.rotation, platform_local_before.rotation) > 0.0001f ||
        glm::distance(platform_transform.scale, platform_local_before.scale) > 0.0001f)
        return fail(name, "kinematic world sync overwrote the authored local transform");

    CharacterControllerState carried_state;
    if (!character_physics.readControllerState(PhysicsTestEcsBridge::entityRef(w, character), carried_state))
        return fail(name, "failed to read carried character state");
    if (!carried_state.grounded || carried_state.ground_velocity.x < 0.1f ||
        character_transform.position.x < character_start_x + 0.25f)
        return fail(name, "CharacterVirtual occupant was not carried by parent-only platform motion");

    // A post-attach invalid ancestor scale must freeze the last valid Jolt
    // pose. Restoring unit scale resumes at the current composed pose.
    parent_transform.scale = {2.0f, 1.0f, 1.0f};
    parent_transform.position += glm::vec3(3.0f, 0.0f, 0.0f);
    w.getPhysicsSystem().syncTransformsToJolt(w.registry);
    w.getPhysicsSystem().syncTransformsToJolt(w.registry);
    if (!pose_matches_query(expected_world, "invalid-scale freeze"))
        return false;

    parent_transform.scale = glm::vec3(1.0f);
    const glm::mat4 resumed_world = Hierarchy::computeWorldMatrix(w.registry, platform);
    w.stepPhysicsSingleStep();
    if (!pose_matches_query(resumed_world, "unit-scale resume"))
        return false;
    if (glm::distance(platform_transform.position, platform_local_before.position) > 0.0001f ||
        glm::distance(platform_transform.rotation, platform_local_before.rotation) > 0.0001f ||
        glm::distance(platform_transform.scale, platform_local_before.scale) > 0.0001f)
        return fail(name, "resumed kinematic sync changed the authored local transform");

    return pass(name);
}

static bool testD6CharacterTeleportRefreshesContacts()
{
    const std::string name = "D6 CharacterController teleport refreshes contacts";
    world w;
    CharacterPhysicsService character_physics(w);
    w.initializePhysics();

    ColliderComponent floor_collider;
    floor_collider.shape_type = ColliderShapeType::Box;
    floor_collider.box_half_extents = glm::vec3(10.0f, 0.5f, 10.0f);
    const auto floor_shape = PhysicsSystem::createShapeFromCollider(floor_collider, glm::vec3(1.0f));
    const entt::entity floor = w.registry.create();
    w.registry.emplace<TransformComponent>(floor, 0.0f, -0.5f, 0.0f);
    w.registry.emplace<RigidBodyComponent>(floor);
    if (!floor_shape)
        return fail(name, "failed to create character floor");
    w.getPhysicsSystem().createStaticBody(
        glm::vec3(0.0f, -0.5f, 0.0f), glm::vec3(0.0f), floor_shape, floor);
    if (!w.getPhysicsSystem().hasBody(floor))
        return fail(name, "failed to create character floor body");

    const entt::entity character = w.registry.create();
    auto& transform = w.registry.emplace<TransformComponent>(character, 0.0f, 1.25f, 0.0f);
    w.registry.emplace<CharacterControllerComponent>(character);
    const EntityRef character_ref = PhysicsTestEcsBridge::entityRef(w, character);
    if (!w.isEntityValid(character_ref) ||
        !w.hasComponent<CharacterControllerComponent>(character_ref) ||
        w.tryGetComponent<TransformComponent>(character_ref) != &transform ||
        w.tryGetComponent<TransformComponent>(EntityRef{}) != nullptr ||
        w.hasComponent<TransformComponent>(EntityRef{}))
        return fail(name, "Garden component lookup failed");
    if (character_physics.createController(EntityRef{}) ||
        character_physics.teleportController(EntityRef{}, glm::vec3(1.0f)) ||
        character_physics.writeControllerState(EntityRef{}, CharacterControllerState{}))
        return fail(name, "null controller reference accepted");
    if (!character_physics.createController(character_ref))
        return fail(name, "failed to create CharacterController");
    for (int tick = 0; tick < 8; ++tick)
    {
        CharacterControllerState step_state;
        if (!character_physics.simulateController(character_ref, CharacterMoveInput{}, w.fixed_delta, step_state))
            return fail(name, "failed to simulate character before teleport");
        w.stepPhysicsSingleStep();
        if (!character_physics.refreshController(character_ref))
            return fail(name, "failed to refresh character contacts");
    }
    CharacterControllerState state;
    if (!character_physics.readControllerState(character_ref, state) || !state.grounded)
        return fail(name, "character did not establish floor contact before teleport");

    const glm::vec3 target(20.0f, 10.0f, 0.0f);
    const glm::vec3 target_rotation(0.0f, 120.0f, 0.0f);
    if (!w.teleport_body(character, target, target_rotation))
        return fail(name, "generic world teleport did not delegate to CharacterVirtual");
    if (!character_physics.readControllerState(character_ref, state))
        return fail(name, "failed to read controller after teleport");
    if (glm::distance(state.position, target) > 0.001f || glm::length(state.velocity) > 0.001f)
        return fail(name, "character teleport did not set position and clear velocity");
    if (state.grounded || glm::length(state.ground_velocity) > 0.001f)
        return fail(name, "character teleport did not refresh support contacts");
    if (glm::distance(transform.rotation, target_rotation) > 0.001f)
        return fail(name, "character teleport did not update ECS rotation");

    const glm::vec3 second_target(22.0f, 9.0f, 0.0f);
    if (!character_physics.teleportController(character_ref, second_target) ||
        !character_physics.readControllerState(character_ref, state) ||
        glm::distance(state.position, second_target) > 0.001f)
        return fail(name, "Garden controller teleport did not move the character");

    w.registry.destroy(character);
    if (w.isEntityValid(character_ref) ||
        w.tryGetComponent<TransformComponent>(character_ref) != nullptr ||
        character_physics.createController(character_ref) ||
        character_physics.teleportController(character_ref, second_target) ||
        character_physics.writeControllerState(character_ref, CharacterControllerState{}))
        return fail(name, "stale controller reference accepted");

    return pass(name);
}

static bool testD6TriggerLifecycle()
{
    const std::string name = "D6 trigger enter exit exactly once";
    EventBus::get().clear();
    g_trigger_entered = 0; g_trigger_exited = 0; g_last_trigger_id = 0;
    EventBus::get().sink<TriggerEnteredEvent>().connect<&onTriggerEntered>();
    EventBus::get().sink<TriggerExitedEvent>().connect<&onTriggerExited>();
    bool okk = true;
    auto check = [&](const char* what, int entered, int exited) {
        EventBus::get().flush();
        if (g_trigger_entered != entered || g_trigger_exited != exited)
        { okk = false; std::cerr << "  lifecycle " << what << ": entered=" << g_trigger_entered << "/" << entered
                                 << " exited=" << g_trigger_exited << "/" << exited << std::endl; }
    };
    {
        world w; w.initializePhysics();
        auto trig = makeTriggerBox(w, glm::vec3(0.0f), 2.0f, 7);
        if (w.getPhysicsSystem().hasBody(trig) == false) { EventBus::get().sink<TriggerEnteredEvent>().disconnect<&onTriggerEntered>(); EventBus::get().sink<TriggerExitedEvent>().disconnect<&onTriggerExited>(); return fail(name, "trigger body was not created"); }
        auto occupant = w.registry.create();
        w.registry.emplace<TransformComponent>(occupant, 10.0f, 0.0f, 0.0f);
        auto& rb = w.registry.emplace<RigidBodyComponent>(occupant);
        rb.motion_type = BodyMotionType::Kinematic; rb.apply_gravity = false;
        w.getPhysicsSystem().createKinematicBody(glm::vec3(10.0f, 0.0f, 0.0f), glm::vec3(0.0f), makeBoxShape(), occupant);
        auto step = [&]() { w.getPhysicsSystem().stepPhysics(w.registry); };
        step(); check("outside", 0, 0);
        w.registry.get<TransformComponent>(occupant).position = glm::vec3(0.0f); // teleport in
        step(); check("teleport in", 1, 0);
        for (int i = 0; i < 600; ++i) step(); // 600 fixed-step ticks
        check("idle inside for 600 ticks (no duplicates)", 1, 0);
        if (g_last_trigger_id != 7) { EventBus::get().sink<TriggerEnteredEvent>().disconnect<&onTriggerEntered>(); EventBus::get().sink<TriggerExitedEvent>().disconnect<&onTriggerExited>(); return fail(name, "trigger_id was not propagated"); }
        w.registry.get<TransformComponent>(occupant).position = glm::vec3(10.0f, 0.0f, 0.0f); // teleport out
        step(); check("teleport out", 1, 1);
        w.registry.get<TransformComponent>(occupant).position = glm::vec3(0.0f);
        step(); check("re-enter", 2, 1);
        // Removing an active trigger body must flush its remembered occupants
        // through exits even though the authored trigger entity remains alive.
        w.getPhysicsSystem().removeBody(trig);
        step(); check("sensor body removed", 2, 2);
        if (!w.registry.get<TriggerOverlapState>(trig).occupants.empty())
            okk = false;

        // Recreate the sensor, enter again, then destroy an occupant inside.
        w.getPhysicsSystem().createTriggerBody(w.registry, trig);
        step(); check("sensor body recreated", 3, 2);
        w.getPhysicsSystem().removeBody(occupant);
        w.registry.destroy(occupant);
        step(); check("destroy inside", 3, 3);
        step(); check("destroy pruned (no extra exit)", 3, 3);
        // Occupant set in state must be empty now.
        if (!w.registry.get<TriggerOverlapState>(trig).occupants.empty())
            okk = false;
    }
    EventBus::get().sink<TriggerEnteredEvent>().disconnect<&onTriggerEntered>();
    EventBus::get().sink<TriggerExitedEvent>().disconnect<&onTriggerExited>();
    EventBus::get().clear();
    if (!okk) return fail(name, "enter/exit counts were not exactly-once (see above)");
    return pass(name);
}

static bool testD6CharacterTriggerLifecycle()
{
    const std::string name = "D6 trigger lifecycle with CharacterController";
    constexpr uint32_t trigger_id = 304;

    EventBus::get().flush<TriggerEnteredEvent>();
    EventBus::get().flush<TriggerExitedEvent>();
    TriggerLifecycleCapture capture;
    entt::scoped_connection entered_connection{
        EventBus::get().sink<TriggerEnteredEvent>().connect<&captureTriggerEntered>(capture) };
    entt::scoped_connection exited_connection{
        EventBus::get().sink<TriggerExitedEvent>().connect<&captureTriggerExited>(capture) };

    world w;
    CharacterPhysicsService character_physics(w);
    w.initializePhysics();

    auto ground = w.registry.create();
    w.registry.emplace<TransformComponent>(ground, 0.0f, -0.5f, 0.0f);
    w.registry.emplace<RigidBodyComponent>(ground);
    ColliderComponent ground_collider;
    ground_collider.shape_type = ColliderShapeType::Box;
    ground_collider.box_half_extents = glm::vec3(20.0f, 0.5f, 20.0f);
    const auto ground_shape = PhysicsSystem::createShapeFromCollider(ground_collider, glm::vec3(1.0f));
    if (!ground_shape || w.getPhysicsSystem().createStaticBody(
            glm::vec3(0.0f, -0.5f, 0.0f), glm::vec3(0.0f), ground_shape, ground).IsInvalid())
        return fail(name, "failed to create character ground");

    const auto trigger = makeTriggerBox(w, glm::vec3(0.0f, 1.0f, 4.0f), 2.0f, trigger_id);
    if (!w.getPhysicsSystem().hasBody(trigger))
        return fail(name, "trigger body was not created");

    const auto character = w.registry.create();
    w.registry.emplace<TransformComponent>(character, 0.0f, 1.25f, 0.0f);
    auto& controller = w.registry.emplace<CharacterControllerComponent>(character);
    controller.move_speed = 4.0f;
    controller.jump_velocity = 3.0f;
    if (w.getPhysicsSystem().createCharacterController(w.registry, character).IsInvalid())
        return fail(name, "failed to create CharacterController");

    auto step = [&](const CharacterMoveInput& input)
    {
        CharacterControllerState state;
        if (!character_physics.simulateController(PhysicsTestEcsBridge::entityRef(w, character), input, w.fixed_delta, state))
            return false;
        w.stepPhysicsSingleStep();
        if (!character_physics.refreshController(PhysicsTestEcsBridge::entityRef(w, character)))
            return false;
        EventBus::get().flush<TriggerEnteredEvent>();
        EventBus::get().flush<TriggerExitedEvent>();
        return true;
    };
    auto is_occupant = [&]()
    {
        const auto& occupants = w.registry.get<TriggerOverlapState>(trigger).occupants;
        return std::find(occupants.begin(), occupants.end(), character) != occupants.end();
    };
    auto check_events = [&](std::size_t enters, std::size_t exits, const char* phase)
    {
        if (capture.entered.size() != enters || capture.exited.size() != exits)
        {
            std::cerr << "  character trigger " << phase << ": entered=" << capture.entered.size()
                      << "/" << enters << " exited=" << capture.exited.size() << "/" << exits << std::endl;
            return false;
        }
        for (const auto& event : capture.entered)
            if (event.trigger != trigger || event.other != character || event.trigger_id != trigger_id)
                return false;
        for (const auto& event : capture.exited)
            if (event.trigger != trigger || event.other != character || event.trigger_id != trigger_id)
                return false;
        return true;
    };

    if (!step(CharacterMoveInput{}))
        return fail(name, "failed to advance character trigger fixture");
    if (is_occupant() || !check_events(0, 0, "initially outside"))
        return fail(name, "character began inside the trigger or emitted an event");

    CharacterMoveInput forward;
    forward.move_forward = 1.0f;
    bool reached_trigger_center = false;
    for (int tick = 0; tick < 120; ++tick)
    {
        if (!step(forward))
            return fail(name, "failed to advance character trigger fixture");
        CharacterControllerState state;
        if (!character_physics.readControllerState(PhysicsTestEcsBridge::entityRef(w, character), state))
            return fail(name, "failed to read trigger fixture character state");
        if (state.position.z >= 4.0f)
        {
            reached_trigger_center = true;
            break;
        }
    }
    if (!reached_trigger_center || !is_occupant() || !check_events(1, 0, "entry by movement"))
        return fail(name, "moving CharacterController did not enter exactly once");

    for (int tick = 0; tick < 600; ++tick)
        if (!step(CharacterMoveInput{}))
            return fail(name, "failed to advance character trigger fixture");
    if (!is_occupant() || !check_events(1, 0, "600 idle fixed ticks"))
        return fail(name, "idle CharacterController emitted duplicate edges or left the trigger");

    CharacterMoveInput backward;
    backward.move_forward = -1.0f;
    bool left_trigger = false;
    for (int tick = 0; tick < 180; ++tick)
    {
        if (!step(backward))
            return fail(name, "failed to advance character trigger fixture");
        if (!is_occupant())
        {
            left_trigger = true;
            break;
        }
    }
    if (!left_trigger || !check_events(1, 1, "leave by movement"))
        return fail(name, "moving CharacterController did not leave with exactly one exit");
    for (int tick = 0; tick < 10; ++tick)
        if (!step(CharacterMoveInput{}))
            return fail(name, "failed to advance character trigger fixture");
    if (is_occupant() || !check_events(1, 1, "idle outside"))
        return fail(name, "outside idle ticks emitted a duplicate edge");

    bool reentered = false;
    for (int tick = 0; tick < 180; ++tick)
    {
        if (!step(forward))
            return fail(name, "failed to advance character trigger fixture");
        if (is_occupant())
        {
            reentered = true;
            break;
        }
    }
    if (!reentered || !check_events(2, 1, "re-entry by movement"))
        return fail(name, "moving CharacterController did not re-enter exactly once");

    w.getPhysicsSystem().removeBody(character);
    w.registry.destroy(character);
    w.stepPhysicsSingleStep();
    EventBus::get().flush<TriggerEnteredEvent>();
    EventBus::get().flush<TriggerExitedEvent>();
    if (!check_events(2, 2, "destroy while inside") ||
        capture.exited.back().other != character ||
        !w.registry.get<TriggerOverlapState>(trigger).occupants.empty())
        return fail(name, "destroyed character did not emit one exit and prune overlap state");

    w.stepPhysicsSingleStep();
    EventBus::get().flush<TriggerEnteredEvent>();
    EventBus::get().flush<TriggerExitedEvent>();
    if (!check_events(2, 2, "destroyed occupant pruned"))
        return fail(name, "destroyed character caused a duplicate event after pruning");

    return pass(name);
}

static bool testD6TriggerCapAndMeshRejection()
{
    const std::string name = "D6 trigger cap and mesh rejection";
    if (PhysicsSystem::kMaxTriggerCount != 256) return fail(name, "soft cap is not 256");

    struct CapturedLog
    {
        spdlog::level::level_enum level;
        std::string message;
    };
    std::vector<CapturedLog> captured_logs;
    auto engine_logger = EE::CLog::GetEngineLogger();
    if (!engine_logger) return fail(name, "engine logger was not initialized");
    auto capture_sink = std::make_shared<spdlog::sinks::callback_sink_mt>(
        [&captured_logs](const spdlog::details::log_msg& msg)
        {
            captured_logs.push_back({ msg.level, std::string(msg.payload.data(), msg.payload.size()) });
        });
    struct SinkRegistration
    {
        std::shared_ptr<spdlog::logger> logger;
        spdlog::sink_ptr sink;
        ~SinkRegistration()
        {
            if (!logger) return;
            auto& sinks = logger->sinks();
            sinks.erase(std::remove(sinks.begin(), sinks.end(), sink), sinks.end());
        }
    } sink_registration{ engine_logger, capture_sink };
    engine_logger->sinks().push_back(capture_sink);

    PhysicsSystemSettings settings; settings.max_trigger_count = 2;
    world w(settings); w.initializePhysics();
    auto t1 = makeTriggerBox(w, glm::vec3(0.0f), 1.0f, 1);
    auto t2 = makeTriggerBox(w, glm::vec3(10.0f, 0.0f, 0.0f), 1.0f, 2);
    if (!w.getPhysicsSystem().hasBody(t1) || !w.getPhysicsSystem().hasBody(t2)) return fail(name, "first triggers failed");
    auto t3 = makeTriggerBox(w, glm::vec3(20.0f, 0.0f, 0.0f), 1.0f, 3);
    if (w.getPhysicsSystem().hasBody(t3)) return fail(name, "trigger beyond cap created a body");
    auto mesh_trig = w.registry.create();
    w.registry.emplace<TransformComponent>(mesh_trig, 0.0f, 5.0f, 0.0f);
    auto& col = w.registry.emplace<ColliderComponent>(mesh_trig);
    col.shape_type = ColliderShapeType::Mesh;
    w.registry.emplace<TriggerComponent>(mesh_trig);
    if (!w.getPhysicsSystem().createTriggerBody(w.registry, mesh_trig).IsInvalid())
        return fail(name, "mesh trigger was not rejected");
    if (w.getPhysicsSystem().hasBody(mesh_trig)) return fail(name, "mesh trigger created a body");

    const auto has_log = [&captured_logs](spdlog::level::level_enum level, const std::string& text)
    {
        return std::any_of(captured_logs.begin(), captured_logs.end(),
            [level, &text](const CapturedLog& entry)
            {
                return entry.level == level && entry.message.find(text) != std::string::npos;
            });
    };
    if (!has_log(spdlog::level::warn, "Mesh/ConvexHull trigger shapes are not supported in v1"))
        return fail(name, "mesh trigger rejection did not produce the expected warning");

    // Exercise the production hard cap with the default-sized trigger budget.
    // Keep all accepted sensors away from the probe so the rejected 257th
    // trigger is the only one that could have reported an enter at the origin.
    captured_logs.clear();
    constexpr uint32_t accepted_trigger_count = 256;
    PhysicsSystemSettings hard_cap_settings;
    if (hard_cap_settings.max_trigger_count != accepted_trigger_count)
        return fail(name, "default trigger budget is not 256");
    world hard_cap_world(hard_cap_settings);
    hard_cap_world.initializePhysics();
    for (uint32_t i = 0; i < accepted_trigger_count; ++i)
    {
        auto trigger = makeTriggerBox(hard_cap_world,
            glm::vec3(1000.0f + float(i) * 10.0f, 0.0f, 0.0f), 1.0f, i + 1);
        if (!hard_cap_world.getPhysicsSystem().hasBody(trigger))
            return fail(name, "one of the first 256 primitive triggers was rejected");
    }

    auto trigger_257 = makeTriggerBox(hard_cap_world, glm::vec3(0.0f), 1.0f, 257);
    if (hard_cap_world.getPhysicsSystem().hasBody(trigger_257))
        return fail(name, "257th enabled primitive trigger created a body");
    if (!hard_cap_world.registry.get<TriggerComponent>(trigger_257).enabled)
        return fail(name, "257th trigger was not enabled");
    if (!has_log(spdlog::level::err, "trigger cap (256) reached, failing closed"))
        return fail(name, "257th trigger rejection did not produce the expected error log");

    EventBus::get().clear();
    g_trigger_entered = 0;
    EventBus::get().sink<TriggerEnteredEvent>().connect<&onTriggerEntered>();
    auto probe = makeDynamicBox(hard_cap_world, glm::vec3(0.0f), true);
    const float probe_y_before = hard_cap_world.registry.get<TransformComponent>(probe).position.y;
    for (int i = 0; i < 3; ++i)
        hard_cap_world.getPhysicsSystem().stepPhysics(hard_cap_world.registry);
    EventBus::get().flush();
    EventBus::get().sink<TriggerEnteredEvent>().disconnect<&onTriggerEntered>();
    EventBus::get().clear();
    if (g_trigger_entered != 0) return fail(name, "failed/capped trigger emitted an enter event");
    if (hard_cap_world.registry.all_of<TriggerOverlapState>(trigger_257))
        return fail(name, "failed/capped trigger created overlap state");
    if (hard_cap_world.registry.get<TransformComponent>(probe).position.y >= probe_y_before - 0.001f)
        return fail(name, "world stopped stepping after the 257th trigger was rejected");
    return pass(name);
}

static bool testD6TriggerOccupantCapacity()
{
    const std::string name = "D6 trigger tracks more than 64 occupants";
    EventBus::get().clear();
    g_trigger_entered = 0;
    EventBus::get().sink<TriggerEnteredEvent>().connect<&onTriggerEntered>();
    {
        world w; w.initializePhysics();
        const auto trigger = makeTriggerBox(w, glm::vec3(0.0f), 5.0f, 9);
        constexpr int occupant_count = 70;
        for (int i = 0; i < occupant_count; ++i)
        {
            const glm::vec3 pos(float(i % 8) * 0.2f, 1.0f + float(i / 8) * 0.2f, 0.0f);
            auto e = w.registry.create();
            w.registry.emplace<TransformComponent>(e, pos.x, pos.y, pos.z);
            auto& rb = w.registry.emplace<RigidBodyComponent>(e);
            rb.motion_type = BodyMotionType::Kinematic;
            rb.apply_gravity = false;
            w.getPhysicsSystem().createKinematicBody(pos, glm::vec3(0.0f), makeBoxShape(), e);
        }
        w.getPhysicsSystem().stepPhysics(w.registry);
        EventBus::get().flush();
        if (g_trigger_entered != occupant_count)
        {
            EventBus::get().sink<TriggerEnteredEvent>().disconnect<&onTriggerEntered>();
            EventBus::get().clear();
            return fail(name, "trigger overlap query truncated the occupant set");
        }

        const auto& first_state = w.registry.get<TriggerOverlapState>(trigger).occupants;
        const std::vector<entt::entity> expected_state = first_state;
        const auto entity_less = [](entt::entity a, entt::entity b) {
            return entt::to_integral(a) < entt::to_integral(b);
        };
        if (expected_state.size() != occupant_count || !std::is_sorted(expected_state.begin(), expected_state.end(), entity_less))
        {
            EventBus::get().sink<TriggerEnteredEvent>().disconnect<&onTriggerEntered>();
            EventBus::get().clear();
            return fail(name, "TriggerOverlapState occupants were not complete and sorted");
        }
        for (int step = 0; step < 12; ++step)
        {
            w.getPhysicsSystem().stepPhysics(w.registry);
            EventBus::get().flush();
            const auto& current_state = w.registry.get<TriggerOverlapState>(trigger).occupants;
            if (g_trigger_entered != occupant_count || current_state != expected_state ||
                !std::is_sorted(current_state.begin(), current_state.end(), entity_less))
            {
                EventBus::get().sink<TriggerEnteredEvent>().disconnect<&onTriggerEntered>();
                EventBus::get().clear();
                return fail(name, "TriggerOverlapState order changed or emitted duplicate enters across fixed steps");
            }
        }
    }
    EventBus::get().sink<TriggerEnteredEvent>().disconnect<&onTriggerEntered>();
    EventBus::get().clear();
    return pass(name);
}

static bool testD6SensorNonInterference()
{
    const std::string name = "D6-T3 sensor non-interference";
    EventBus::get().clear();

    world control_world;
    world sensor_world;
    CharacterPhysicsService control_physics(control_world);
    CharacterPhysicsService sensor_physics(sensor_world);
    entt::entity control_player = entt::null;
    entt::entity sensor_player = entt::null;
    entt::entity unused_trigger = entt::null;
    entt::entity sensor_trigger = entt::null;
    auto setup_scene = [](world& w, bool with_trigger, entt::entity& player, entt::entity& trigger)
    {
        w.initializePhysics();

        auto ground = w.registry.create();
        w.registry.emplace<TransformComponent>(ground, 0.0f, -0.5f, 0.0f);
        w.registry.emplace<RigidBodyComponent>(ground);
        ColliderComponent ground_collider;
        ground_collider.shape_type = ColliderShapeType::Box;
        ground_collider.box_half_extents = glm::vec3(20.0f, 0.5f, 20.0f);
        const auto ground_shape = PhysicsSystem::createShapeFromCollider(
            ground_collider, glm::vec3(1.0f));
        if (!ground_shape || w.getPhysicsSystem().createStaticBody(
                glm::vec3(0.0f, -0.5f, 0.0f), glm::vec3(0.0f), ground_shape, ground).IsInvalid())
            return false;

        player = w.registry.create();
        w.registry.emplace<TransformComponent>(player, 0.0f, 1.25f, 0.0f);
        auto& controller = w.registry.emplace<CharacterControllerComponent>(player);
        controller.move_speed = 4.0f;
        controller.jump_velocity = 3.0f;
        if (w.getPhysicsSystem().createCharacterController(w.registry, player).IsInvalid())
            return false;

        if (with_trigger)
        {
            trigger = makeTriggerBox(w, glm::vec3(0.0f, 1.0f, 4.0f), 2.0f, 303);
            if (!w.getPhysicsSystem().hasBody(trigger))
                return false;
        }
        return true;
    };

    if (!setup_scene(control_world, false, control_player, unused_trigger) ||
        !setup_scene(sensor_world, true, sensor_player, sensor_trigger))
    {
        EventBus::get().clear();
        return fail(name, "failed to create matching character scenes and trigger body");
    }

    auto same_float_bits = [](float a, float b)
    {
        return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b);
    };
    auto same_vec3_bits = [&](const glm::vec3& a, const glm::vec3& b)
    {
        return same_float_bits(a.x, b.x) && same_float_bits(a.y, b.y) && same_float_bits(a.z, b.z);
    };
    auto same_motion_trace = [&](const CharacterControllerState& a, const CharacterControllerState& b)
    {
        return same_vec3_bits(a.position, b.position) && same_vec3_bits(a.velocity, b.velocity);
    };

    bool sensor_overlapped_character = false;
    for (int tick = 0; tick < 270; ++tick)
    {
        CharacterMoveInput input;
        if (tick >= 30 && tick < 180)
            input.move_forward = 1.0f;
        else if (tick >= 180 && tick < 210)
        {
            input.move_forward = 0.6f;
            input.move_right = 0.4f;
        }
        else if (tick >= 210 && tick < 240)
            input.move_forward = -0.5f;
        if (tick >= 90 && tick < 100)
            input.buttons |= CharacterMoveFlags::Jump;

        CharacterControllerState control_step;
        if (!control_physics.simulateController(
                PhysicsTestEcsBridge::entityRef(control_world, control_player), input, control_world.fixed_delta, control_step))
            return fail(name, "failed to simulate control character");
        CharacterControllerState sensor_step;
        if (!sensor_physics.simulateController(
                PhysicsTestEcsBridge::entityRef(sensor_world, sensor_player), input, sensor_world.fixed_delta, sensor_step))
            return fail(name, "failed to simulate sensor character");
        if (!same_motion_trace(control_step, sensor_step))
        {
            EventBus::get().clear();
            return fail(name, "character movement diverged during controller simulation at tick " +
                std::to_string(tick));
        }

        control_world.getPhysicsSystem().stepPhysics(control_world.registry);
        sensor_world.getPhysicsSystem().stepPhysics(sensor_world.registry);
        if (!control_physics.refreshController(PhysicsTestEcsBridge::entityRef(control_world, control_player)))
            return fail(name, "failed to refresh control character");
        if (!sensor_physics.refreshController(PhysicsTestEcsBridge::entityRef(sensor_world, sensor_player)))
            return fail(name, "failed to refresh sensor character");

        CharacterControllerState control_state;
        if (!control_physics.readControllerState(PhysicsTestEcsBridge::entityRef(control_world, control_player), control_state))
            return fail(name, "failed to read control character");
        CharacterControllerState sensor_state;
        if (!sensor_physics.readControllerState(PhysicsTestEcsBridge::entityRef(sensor_world, sensor_player), sensor_state))
            return fail(name, "failed to read sensor character");
        if (!same_motion_trace(control_state, sensor_state))
        {
            EventBus::get().clear();
            return fail(name, "character movement diverged after physics step at tick " +
                std::to_string(tick));
        }

        const auto& occupants = sensor_world.registry.get<TriggerOverlapState>(sensor_trigger).occupants;
        if (std::find(occupants.begin(), occupants.end(), sensor_player) != occupants.end())
            sensor_overlapped_character = true;
    }

    EventBus::get().clear();
    if (!sensor_overlapped_character)
        return fail(name, "character never overlapped the sensor during the movement trace");
    return pass(name);
}

// Private test bridge for building a collision scene and returning only a Garden handle.
static EntityRef createMovingJumpFixture(world& w)
{
    w.initializePhysics();
    const entt::entity ground = w.registry.create();
    w.registry.emplace<TransformComponent>(ground, 0.0f, -0.5f, 0.0f);
    w.registry.emplace<RigidBodyComponent>(ground);
    ColliderComponent collider;
    collider.shape_type = ColliderShapeType::Box;
    collider.box_half_extents = glm::vec3(50.0f, 0.5f, 50.0f);
    const auto shape = PhysicsSystem::createShapeFromCollider(collider, glm::vec3(1.0f));
    if (!shape || w.getPhysicsSystem().createStaticBody(glm::vec3(0.0f, -0.5f, 0.0f),
            glm::vec3(0.0f), shape, ground).IsInvalid())
        return {};

    const entt::entity character = w.registry.create();
    w.registry.emplace<TransformComponent>(character, 0.0f, 1.2f, 0.0f);
    auto& rigid_body = w.registry.emplace<RigidBodyComponent>(character);
    rigid_body.apply_gravity = false;
    auto& player = w.registry.emplace<PlayerComponent>(character);
    player.speed = 4.0f;
    player.jump_force = 3.0f;
    auto& controller = w.registry.emplace<CharacterControllerComponent>(character);
    controller.move_speed = player.speed;
    controller.jump_velocity = player.jump_force;
    const EntityRef ref = PhysicsTestEcsBridge::entityRef(w, character);
    CharacterPhysicsService character_physics(w);
    return character_physics.createController(ref) ? ref : EntityRef{};
}

static bool testCharacterPhysicsServiceControllerAvailability()
{
    const std::string name = "character physics service reports runtime controller availability";
    PhysicsSystemSettings settings;
    settings.worker_thread_count = 1;
    world w(settings);
    const entt::entity entity = w.registry.create();
    w.registry.emplace<TransformComponent>(entity, 0.0f, 2.0f, 0.0f);
    w.registry.emplace<CharacterControllerComponent>(entity);
    const EntityRef ref = PhysicsTestEcsBridge::entityRef(w, entity);
    CharacterPhysicsService physics(w);
    CharacterControllerState state;
    if (physics.hasController(EntityRef{}) || physics.hasController(ref) ||
        physics.readControllerState(ref, state))
        return fail(name, "component without runtime controller reported available");
    if (!physics.createController(ref) || !physics.hasController(ref) ||
        !physics.readControllerState(ref, state))
        return fail(name, "created runtime controller was not available");
    w.registry.remove<TransformComponent>(entity);
    if (physics.hasController(ref) || physics.readControllerState(ref, state))
        return fail(name, "controller without required transform reported available");
    return pass(name);
}

static bool testPlayerControllerBufferedMovingJump()
{
    const std::string name = "player controller consumes buffered moving jump on fixed step";
    PhysicsSystemSettings settings;
    settings.worker_thread_count = 1;
    world w(settings);
    CharacterPhysicsService character_physics(w);
    const EntityRef pawn = createMovingJumpFixture(w);
    if (pawn.isNull())
        return fail(name, "failed to create character fixture");

    const entt::entity entity = resolveEntityRef(w.registry, pawn);
    auto& local = w.registry.emplace<LocalPlayerInputComponent>(entity);
    local.move.y = 1.0f;
    PlayerController controller(nullptr, &w);
    controller.setPossessedPlayer(pawn);
    for (int tick = 0; tick < 30; ++tick)
        controller.update(w.fixed_delta);

    CharacterControllerState before;
    if (!character_physics.readControllerState(pawn, before))
        return fail(name, "failed to read before controller state");
    if (!before.grounded || glm::length(glm::vec2(before.velocity.x, before.velocity.z)) < 0.1f)
        return fail(name, "fixture did not reach grounded forward movement");

    // A released tap arrives before a render update too short to simulate.
    local.jump_held = false;
    local.jump_press_pending = true;
    controller.update(w.fixed_delta * 0.5f);
    CharacterControllerState pending;
    if (!character_physics.readControllerState(pawn, pending))
        return fail(name, "failed to read pending controller state");
    if (!local.jump_press_pending || glm::distance(before.position, pending.position) > 0.0001f)
        return fail(name, "jump press was consumed before a fixed step");

    controller.update(w.fixed_delta * 0.5f);
    CharacterControllerState jumped;
    if (!character_physics.readControllerState(pawn, jumped))
        return fail(name, "failed to read jumped controller state");
    if (local.jump_press_pending || jumped.position.y <= before.position.y + 0.02f ||
        jumped.velocity.y < 1.0f ||
        glm::length(glm::vec2(jumped.velocity.x, jumped.velocity.z)) < 0.1f)
        return fail(name, "fixed step did not consume the press and jump while moving");

    return pass(name);
}

static bool testServerClientCharacterMovementParity()
{
    const std::string name = "server/client same-build CharacterVirtual movement parity";
    constexpr int tick_count = 600;
    constexpr float max_position_error_m = 0.05f;

    PhysicsSystemSettings settings;
    settings.worker_thread_count = 1;
    world server_world(settings);
    world client_world(settings);
    CharacterPhysicsService server_physics(server_world);
    CharacterPhysicsService client_physics(client_world);
    entt::entity server_character = entt::null;
    entt::entity client_character = entt::null;

    auto add_static_box = [](world& w, const glm::vec3& position, const glm::vec3& half_extents)
    {
        const entt::entity entity = w.registry.create();
        w.registry.emplace<TransformComponent>(entity, position.x, position.y, position.z);
        w.registry.emplace<RigidBodyComponent>(entity);
        ColliderComponent collider;
        collider.shape_type = ColliderShapeType::Box;
        collider.box_half_extents = half_extents;
        const auto shape = PhysicsSystem::createShapeFromCollider(collider, glm::vec3(1.0f));
        return shape && !w.getPhysicsSystem().createStaticBody(
            position, glm::vec3(0.0f), shape, entity).IsInvalid();
    };

    auto setup_scene = [&](world& w, entt::entity& character)
    {
        w.initializePhysics();
        if (!add_static_box(w, glm::vec3(0.0f, -0.5f, 0.0f), glm::vec3(50.0f, 0.5f, 50.0f)))
            return false;
        // The short obstacle exercises CharacterVirtual collision/step handling
        // while leaving enough open floor for the full input script.
        if (!add_static_box(w, glm::vec3(0.0f, 0.15f, 4.0f), glm::vec3(1.5f, 0.15f, 0.35f)))
            return false;

        character = w.registry.create();
        auto& transform = w.registry.emplace<TransformComponent>(character, 0.0f, 1.2f, 0.0f);
        auto& rigid_body = w.registry.emplace<RigidBodyComponent>(character);
        rigid_body.apply_gravity = false;
        auto& player = w.registry.emplace<PlayerComponent>(character);
        player.speed = 4.0f;
        player.jump_force = 3.0f;
        auto& controller = w.registry.emplace<CharacterControllerComponent>(character);
        controller.move_speed = player.speed;
        controller.jump_velocity = player.jump_force;

        if (w.getPhysicsSystem().createCharacterController(w.registry, character).IsInvalid())
            return false;

        CharacterPhysicsService character_physics(w);
        // Both prediction and authority begin with the same resolved contact
        // state, as they would after spawning the pawn into this fixture.
        CharacterControllerState initial;
        if (!character_physics.readControllerState(PhysicsTestEcsBridge::entityRef(w, character), initial))
            return false;
        player.grounded = initial.grounded;
        player.ground_normal = initial.ground_normal;
        transform.position = initial.position;
        rigid_body.velocity = initial.velocity;
        return true;
    };

    if (!setup_scene(server_world, server_character) ||
        !setup_scene(client_world, client_character))
        return fail(name, "failed to create matching independent Jolt scenes");

    // Compare actual takeoff, not just agreement between two equally wrong traces.
    // Exercise forward and diagonal movement on independent authority/prediction scenes.
    for (float strafe : {0.0f, 1.0f})
    {
        world authority(settings);
        world prediction(settings);
        CharacterPhysicsService authority_physics(authority);
        CharacterPhysicsService prediction_physics(prediction);
        const EntityRef authority_ref = createMovingJumpFixture(authority);
        const EntityRef prediction_ref = createMovingJumpFixture(prediction);
        if (authority_ref.isNull() || prediction_ref.isNull())
            return fail(name, "failed to create moving jump scenes");
        CharacterMoveInput movement;
        movement.move_forward = 1.0f;
        movement.move_right = strafe;
        for (int tick = 0; tick < 30; ++tick)
        {
            CharacterControllerState authority_state;
            if (!authority_physics.simulateController(authority_ref, movement, authority.fixed_delta, authority_state))
                return fail(name, "failed to simulate authority moving jump");
            CharacterControllerState prediction_state;
            if (!prediction_physics.simulateController(prediction_ref, movement, prediction.fixed_delta, prediction_state))
                return fail(name, "failed to simulate prediction moving jump");
        }
        CharacterControllerState before;
        if (!authority_physics.readControllerState(authority_ref, before))
            return fail(name, "failed to read moving jump state");
        if (!before.grounded)
            return fail(name, "moving jump fixture did not reach grounded state");
        movement.buttons = CharacterMoveFlags::Jump;
        CharacterControllerState jump;
        if (!authority_physics.simulateController(authority_ref, movement, authority.fixed_delta, jump))
            return fail(name, "failed to simulate authority moving jump");
        CharacterControllerState predicted_jump;
        if (!prediction_physics.simulateController(prediction_ref, movement, prediction.fixed_delta, predicted_jump))
            return fail(name, "failed to simulate prediction moving jump");
        if (jump.position.y <= before.position.y + 0.02f || jump.velocity.y < 1.0f ||
            !jump.jumped || !jump.jump_was_held ||
            glm::distance(jump.position, predicted_jump.position) > 0.001f)
            return fail(name, "moving jump did not take off or prediction diverged");
        float apex = jump.position.y;
        for (int tick = 0; tick < 20; ++tick)
        {
            if (tick == 12)
                movement.buttons = 0;
            // Prediction applies the current pose before simulating each input,
            // preserving the controller's lifecycle fields just as the client does.
            CharacterControllerState prediction_state;
            if (!prediction_physics.readControllerState(prediction_ref, prediction_state) ||
                !prediction_physics.writeControllerState(prediction_ref, prediction_state))
                return fail(name, "prediction state could not be restored");
            CharacterControllerState step;
            if (!authority_physics.simulateController(authority_ref, movement, authority.fixed_delta, step))
                return fail(name, "failed to simulate authority moving jump");
            CharacterControllerState predicted;
            if (!prediction_physics.simulateController(prediction_ref, movement, prediction.fixed_delta, predicted))
                return fail(name, "failed to simulate prediction moving jump");
            apex = std::max(apex, step.position.y);
            if (glm::distance(step.position, predicted.position) > 0.001f ||
                step.jump_was_held != predicted.jump_was_held ||
                step.jump_was_held != (tick < 12))
                return fail(name, "moving jump prediction or held state diverged during flight");
        }
        if (apex < before.position.y + 0.2f)
            return fail(name, "moving jump failed to rise above the ground");
    }

    auto scripted_input = [](int tick)
    {
        CharacterMoveInput input;
        if (tick < 120)
        {
            input.move_forward = 1.0f;
        }
        else if (tick < 180)
        {
            input.move_forward = 0.75f;
            input.move_right = 0.25f;
            input.camera_yaw = 0.35f;
        }
        else if (tick < 195)
        {
            input.move_forward = 1.0f;
            input.buttons |= CharacterMoveFlags::Jump;
        }
        else if (tick < 240)
        {
            input.move_forward = 0.6f;
            input.move_right = 0.4f;
            input.camera_yaw = 0.35f;
        }
        else if (tick < 300)
        {
            input.move_forward = -0.75f;
            input.move_right = 0.5f;
            input.camera_yaw = -0.7f;
        }
        else if (tick < 360)
        {
            // Deliberate neutral interval checks that stopping and gravity
            // remain aligned after the earlier contacts and jump.
        }
        else if (tick < 420)
        {
            input.move_forward = 1.0f;
            input.camera_yaw = glm::half_pi<float>();
        }
        else if (tick < 480)
        {
            input.move_forward = 0.8f;
            input.camera_yaw = glm::pi<float>();
        }
        else if (tick < 490)
        {
            input.move_forward = 1.0f;
            input.camera_yaw = glm::pi<float>();
            input.buttons |= CharacterMoveFlags::Jump;
        }
        else if (tick < 540)
        {
            input.move_forward = 0.4f;
            input.move_right = -1.0f;
            input.camera_yaw = 2.5f;
        }
        return input;
    };

    auto client_start_state = [](world& w, entt::entity entity, CharacterControllerState& state)
    {
        const auto& transform = w.registry.get<TransformComponent>(entity);
        const auto& rigid_body = w.registry.get<RigidBodyComponent>(entity);
        const auto& player = w.registry.get<PlayerComponent>(entity);
        CharacterPhysicsService character_physics(w);
        if (!character_physics.readControllerState(PhysicsTestEcsBridge::entityRef(w, entity), state))
            return false;
        state.position = transform.position;
        state.velocity = rigid_body.velocity;
        state.grounded = player.grounded;
        state.ground_normal = player.ground_normal;
        // Mirror prediction's pose update while retaining controller history.
        return true;
    };

    auto write_network_state = [](world& w, entt::entity entity, const CharacterControllerState& state)
    {
        w.registry.get<TransformComponent>(entity).position = state.position;
        auto& rigid_body = w.registry.get<RigidBodyComponent>(entity);
        rigid_body.velocity = state.velocity;
        auto& player = w.registry.get<PlayerComponent>(entity);
        player.grounded = state.grounded;
        player.ground_normal = state.ground_normal;
    };

    float maximum_divergence = 0.0f;
    int maximum_divergence_tick = -1;
    auto measure_divergence = [&](int tick)
    {
        const glm::vec3 server_position = server_world.registry.get<TransformComponent>(server_character).position;
        const glm::vec3 client_position = client_world.registry.get<TransformComponent>(client_character).position;
        const float divergence = glm::distance(server_position, client_position);
        if (!std::isfinite(divergence))
            return false;
        if (divergence > maximum_divergence)
        {
            maximum_divergence = divergence;
            maximum_divergence_tick = tick;
        }
        return divergence <= max_position_error_m;
    };

    for (int tick = 0; tick < tick_count; ++tick)
    {
        const CharacterMoveInput input = scripted_input(tick);

        // ServerNetworkManager advances the existing CharacterVirtual directly.
        CharacterControllerState server_state;
        if (!server_physics.simulateController(
                PhysicsTestEcsBridge::entityRef(server_world, server_character), input, server_world.fixed_delta, server_state))
            return fail(name, "failed to simulate server character");
        write_network_state(server_world, server_character, server_state);

        // GameModule restores its ECS prediction state before advancing the
        // same CharacterVirtual path for the corresponding input sample.
        CharacterControllerState client_start;
        if (!client_start_state(client_world, client_character, client_start) ||
            !client_physics.writeControllerState(
                PhysicsTestEcsBridge::entityRef(client_world, client_character), client_start))
            return fail(name, "client-side CharacterVirtual state restore failed at tick " +
                std::to_string(tick));
        CharacterControllerState client_state;
        if (!client_physics.simulateController(
                PhysicsTestEcsBridge::entityRef(client_world, client_character), input, client_world.fixed_delta, client_state))
            return fail(name, "failed to simulate client character");
        write_network_state(client_world, client_character, client_state);

        if (!measure_divergence(tick))
            return fail(name, "position divergence exceeded 0.05 m during controller simulation at tick " +
                std::to_string(tick) + " (max " + std::to_string(maximum_divergence) + " m)");

        server_world.stepPhysicsSingleStep();
        client_world.stepPhysicsSingleStep();
        if (!server_physics.refreshController(PhysicsTestEcsBridge::entityRef(server_world, server_character)))
            return fail(name, "failed to refresh server character");
        if (!client_physics.refreshController(PhysicsTestEcsBridge::entityRef(client_world, client_character)))
            return fail(name, "failed to refresh client character");

        if (!measure_divergence(tick))
            return fail(name, "position divergence exceeded 0.05 m after the fixed physics step at tick " +
                std::to_string(tick) + " (max " + std::to_string(maximum_divergence) + " m)");
    }

    return pass(name + ": max position divergence " + std::to_string(maximum_divergence) +
        " m at tick " + std::to_string(maximum_divergence_tick) + " across 600 ticks");
}

static bool testPredictionReplayPreservesCharacterCollision()
{
    const std::string name = "prediction replay preserves CharacterVirtual wall collision";
    constexpr int replay_input_count = 90;

    world w;
    CharacterPhysicsService character_physics(w);
    w.initializePhysics();
    auto add_static_box = [](world& target, const glm::vec3& position, const glm::vec3& half_extents)
    {
        const entt::entity entity = target.registry.create();
        target.registry.emplace<TransformComponent>(entity, position.x, position.y, position.z);
        target.registry.emplace<RigidBodyComponent>(entity);
        ColliderComponent collider;
        collider.shape_type = ColliderShapeType::Box;
        collider.box_half_extents = half_extents;
        const auto shape = PhysicsSystem::createShapeFromCollider(collider, glm::vec3(1.0f));
        return shape && !target.getPhysicsSystem().createStaticBody(
            position, glm::vec3(0.0f), shape, entity).IsInvalid();
    };

    if (!add_static_box(w, glm::vec3(0.0f, -0.5f, 0.0f), glm::vec3(10.0f, 0.5f, 10.0f)))
        return fail(name, "failed to create replay-test floor");
    if (!add_static_box(w, glm::vec3(0.0f, 2.0f, 2.0f), glm::vec3(2.0f, 2.0f, 0.25f)))
        return fail(name, "failed to create replay-test wall");

    const entt::entity character = w.registry.create();
    w.registry.emplace<TransformComponent>(character, 0.0f, 1.2f, 0.0f);
    auto& controller = w.registry.emplace<CharacterControllerComponent>(character);
    controller.move_speed = 4.0f;
    controller.jump_velocity = 3.0f;
    if (w.getPhysicsSystem().createCharacterController(w.registry, character).IsInvalid())
        return fail(name, "failed to create replay-test CharacterController");

    CharacterControllerState authoritative;
    if (!character_physics.readControllerState(PhysicsTestEcsBridge::entityRef(w, character), authoritative))
        return fail(name, "failed to read authoritative character state");
    authoritative.position = glm::vec3(0.0f, 1.2f, 0.0f);
    authoritative.velocity = glm::vec3(0.0f);
    if (!character_physics.writeControllerState(PhysicsTestEcsBridge::entityRef(w, character), authoritative))
        return fail(name, "failed to restore authoritative CharacterController state");

    Net::MovementState shared_state;
    shared_state.position = authoritative.position;
    shared_state.velocity = authoritative.velocity;
    shared_state.grounded = authoritative.grounded;
    shared_state.ground_normal = authoritative.ground_normal;
    Net::MovementConfig shared_config;
    shared_config.speed = controller.move_speed;
    shared_config.jump_force = controller.jump_velocity;
    shared_config.fixed_delta = w.fixed_delta;

    Net::InputRingBuffer<128> history;
    for (int tick = 0; tick < replay_input_count; ++tick)
    {
        Net::PredictionEntry entry;
        entry.tick = static_cast<uint32_t>(tick + 1);
        entry.input.move_forward = 1.0f;
        entry.valid = true;
        history.push(entry);
    }

    bool replay_succeeded = true;
    history.forEachFrom(0, [&](const Net::PredictionEntry& entry) {
        CharacterMoveInput character_input;
        character_input.move_forward = entry.input.move_forward;
        CharacterControllerState replay_state;
        if (!character_physics.simulateController(PhysicsTestEcsBridge::entityRef(w, character), character_input, w.fixed_delta, replay_state))
        {
            replay_succeeded = false;
            return;
        }
        w.stepPhysicsSingleStep();
    });
    if (!replay_succeeded)
        return fail(name, "failed to simulate character replay");
    shared_state = history.replayFrom(0, shared_state,
        [&](const Net::MovementInput& input, const Net::MovementState& start_state) {
            return Net::SharedMovement::simulate(input, start_state, shared_config);
        });

    CharacterControllerState controller_state;
    if (!character_physics.readControllerState(PhysicsTestEcsBridge::entityRef(w, character), controller_state))
        return fail(name, "failed to read character replay state");
    const glm::vec3 controller_position = controller_state.position;
    if (controller_position.z > 1.6f)
        return fail(name, "controller replay passed through the wall to z=" +
            std::to_string(controller_position.z));
    if (shared_state.position.z < 3.0f)
        return fail(name, "collision-free SharedMovement did not pass through the wall to z=" +
            std::to_string(shared_state.position.z));

    return pass(name + ": CharacterVirtual stopped at z=" + std::to_string(controller_position.z) +
        ", SharedMovement reached z=" + std::to_string(shared_state.position.z));
}

static bool containsLayerKey(const nlohmann::json& value)
{
    if (value.is_object())
    {
        for (auto it = value.begin(); it != value.end(); ++it)
        {
            std::string normalized_key;
            for (unsigned char c : it.key())
            {
                if (std::isalnum(c))
                    normalized_key.push_back(static_cast<char>(std::tolower(c)));
            }
            const bool has_layer_name =
                normalized_key.find("objectlayer") != std::string::npos ||
                normalized_key.find("physicslayer") != std::string::npos ||
                normalized_key.find("collisionlayer") != std::string::npos ||
                (normalized_key.find("layer") != std::string::npos &&
                 normalized_key.find("player") == std::string::npos);
            if (has_layer_name || containsLayerKey(it.value()))
                return true;
        }
    }
    else if (value.is_array())
    {
        for (const auto& item : value)
            if (containsLayerKey(item))
                return true;
    }
    return false;
}

static bool testD6LevelRoundTrip()
{
    const std::string name = "D6-U1 level round-trip (no persisted layer indices)";
    // D6-U1 experiment result: grep of LevelManager/scene serialisation and
    // FPSShooter *.level.json found no persisted object-layer values; bodies
    // are recreated from EntityType/motion_type, layers assigned at creation.
    // This round-trip proves save->load preserves that behaviour under 0/1.
    LevelData data; data.metadata.level_name = "D6RoundTrip";
    LevelEntity ground; ground.name = "ground"; ground.type = EntityType::Collidable;
    ground.position = glm::vec3(0.0f, 0.0f, 0.0f);
    ground.has_collider = true; ground.collider_shape_type = "Box";
    ground.collider_box_half_extents = glm::vec3(5.0f, 0.5f, 5.0f);
    data.entities.push_back(ground);
    LevelEntity crate; crate.name = "crate"; crate.type = EntityType::Physical;
    crate.position = glm::vec3(0.0f, 2.0f, 0.0f);
    crate.has_rigidbody = true; crate.mass = 1.0f; crate.apply_gravity = true;
    crate.body_motion_type = "Dynamic";
    crate.has_collider = true; crate.collider_shape_type = "Box";
    crate.collider_box_half_extents = glm::vec3(0.5f);
    data.entities.push_back(crate);

    fs::path tmp = fs::temp_directory_path() / "garden_d6_roundtrip.level.json";
    LevelManager lm;
    if (!lm.saveLevelToJSON(tmp.string(), data)) return fail(name, "saveLevelToJSON failed");
    std::ifstream in(tmp); std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const nlohmann::json saved = nlohmann::json::parse(text, nullptr, false);
    if (saved.is_discarded()) return fail(name, "saved JSON level could not be parsed");
    if (containsLayerKey(saved)) return fail(name, "saved level unexpectedly persists a physics layer field");
    LevelData loaded;
    if (!lm.loadLevelFromJSON(tmp.string(), loaded)) return fail(name, "loadLevelFromJSON failed");
    if (loaded.entities.size() != 2) return fail(name, "round-trip entity count changed");
    world w; w.initializePhysics(); w.setGravity(glm::vec3(0.0f, -1.0f, 0.0f));
    LevelManager lm2;
    if (!lm2.instantiateLevel(loaded, w, nullptr)) return fail(name, "instantiate of round-tripped level failed");
    for (int i = 0; i < 120; ++i) w.getPhysicsSystem().stepPhysics(w.registry);
    bool rested = false;
    auto view = w.registry.view<TagComponent, TransformComponent>();
    for (auto e : view)
        if (view.get<TagComponent>(e).name == "crate")
            rested = approx(view.get<TransformComponent>(e).position.y, 1.0f, 0.2f);
    if (!rested) return fail(name, "dynamic crate did not rest on static ground after round-trip");
    return pass(name);
}

static bool testD6LayerRenumberRegression(const fs::path& repo_root)
{
    const std::string name = "D6-T1 legacy/current character layer comparison";
    const fs::path template_assets = repo_root / "Templates" / "FPSShooter" / "assets";
    const fs::path source_level = template_assets / "levels" / "physics.level.json";
    const fs::path original_cache = template_assets / "models" / "ground_plane.gardenphys";

    auto read_bytes = [](const fs::path& path, std::vector<char>& bytes)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        return !in.bad();
    };

    std::vector<char> original_cache_before;
    if (!fs::exists(source_level) || !read_bytes(original_cache, original_cache_before))
        return fail(name, "FPSShooter physics fixture or its cooked ground cache is missing/unreadable");
    if (original_cache_before.empty())
        return fail(name, "cooked ground cache is empty");

    const fs::path fixture_root = fs::temp_directory_path() /
        ("garden_physics_d6t1_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code fixture_error;
    if (!fs::create_directories(fixture_root / "models", fixture_error))
        return fail(name, "failed to create isolated fixture directory: " + fixture_error.message());
    struct ScopedDirectoryCleanup
    {
        fs::path path;
        ~ScopedDirectoryCleanup()
        {
            std::error_code error;
            fs::remove_all(path, error);
        }
    } cleanup{fixture_root};

    // The authored level is retained, but its model paths resolve to private
    // copies with metadata and collision cooked by the current contract.
    Assets::AssetScanner scanner;
    for (const char* filename : {"ground_plane.obj", "grasscube.obj"})
    {
        const fs::path fixture_model = fixture_root / "models" / filename;
        if (!fs::copy_file(template_assets / "models" / filename, fixture_model,
                           fs::copy_options::none, fixture_error))
            return fail(name, "failed to copy fixture model: " + fixture_error.message());
        if (!scanner.processAsset(fixture_model.string()))
            return fail(name, "failed to generate current metadata for " + std::string(filename));
    }
    const fs::path cooked_cache = fixture_root / "models" / "ground_plane.gardenphys";
    const fs::path round_trip_path = fixture_root / "physics.level.json";
    std::vector<char> cooked_before;
    if (!read_bytes(cooked_cache, cooked_before) || cooked_before.empty())
        return fail(name, "generated cooked ground cache is missing/unreadable");

    Assets::AssetManager::get().setAssetRoot(fixture_root.string());
    Assets::AssetManager::get().setAssetPrefix("assets");
    HeadlessRenderAPI render_api;
    Assets::AssetManager::get().initialize(&render_api);

    LevelManager level_manager;
    LevelData source_data;
    if (!level_manager.loadLevel(source_level.string(), source_data))
        return fail(name, "failed to load FPSShooter physics.level.json");
    std::error_code remove_error;
    fs::remove(round_trip_path, remove_error);
    if (!level_manager.saveLevelToJSON(round_trip_path.string(), source_data))
        return fail(name, "failed to save the serialized physics-level fixture");
    LevelData round_tripped_data;
    if (!level_manager.loadLevel(round_trip_path.string(), round_tripped_data))
        return fail(name, "failed to reload the saved physics-level fixture");
    if (round_tripped_data.entities.size() != source_data.entities.size())
        return fail(name, "level save/load changed the serialized entity count");
    for (size_t i = 0; i < source_data.entities.size(); ++i)
    {
        const auto& source = source_data.entities[i];
        const auto& loaded = round_tripped_data.entities[i];
        if (loaded.name != source.name || loaded.type != source.type ||
            loaded.position != source.position || loaded.rotation != source.rotation ||
            loaded.scale != source.scale || loaded.has_rigidbody != source.has_rigidbody ||
            loaded.has_collider != source.has_collider || loaded.has_constraint != source.has_constraint ||
            (source.has_collider && loaded.collider_shape_type != source.collider_shape_type))
            return fail(name, "level save/load changed authored physics or transform data for '" + source.name + "'");
    }

    struct NamedPosition
    {
        std::string name;
        glm::vec3 position;
    };
    std::vector<NamedPosition> legacy_body_positions;
    std::vector<NamedPosition> current_body_positions;
    glm::vec3 legacy_character_position{0.0f};
    glm::vec3 current_character_position{0.0f};
    CharacterControllerState legacy_character_state;
    CharacterControllerState current_character_state;

    std::string scene_error;
    auto run_scene = [&](bool legacy_mapping,
                         std::vector<NamedPosition>& body_positions,
                         glm::vec3& character_position,
                         CharacterControllerState& character_state)
    {
        auto scene_fail = [&](const std::string& reason) { scene_error = reason; return false; };
        PhysicsSystemSettings settings;
        // Model the historical CharacterVirtual assignment from 1b051986^:
        // the inner body and its query layer were DynamicProp (1). This uses
        // today's two-layer-equivalent pair rules with only StaticWorld and
        // DynamicProp bodies from the serialized fixture. It is a behavioral
        // mapping comparison, not a run of the historical source revision.
        if (legacy_mapping)
            settings.layers.character_body = settings.layers.dynamic_body;

        world w(settings);
        CharacterPhysicsService character_physics(w);
        w.initializePhysics();
        LevelManager instance_manager;
        ReflectionRegistry reflection;
        registerEngineReflection(reflection);
        instance_manager.setReflectionRegistry(&reflection);
        std::vector<entt::entity> level_entities;
        if (!instance_manager.instantiateLevel(round_tripped_data, w, &render_api,
                                               nullptr, nullptr, nullptr, false, &level_entities))
            return scene_fail("level instantiation failed");

        entt::entity ground = entt::null;
        for (entt::entity entity : level_entities)
        {
            if (!w.registry.all_of<TagComponent>(entity)) continue;
            if (w.registry.get<TagComponent>(entity).name == "ground")
                ground = entity;
        }
        if (ground == entt::null) return scene_fail("ground entity missing");
        if (!w.getPhysicsSystem().hasBody(ground)) return scene_fail("ground physics body missing");
        if (!w.registry.all_of<MeshComponent>(ground)) return scene_fail("ground mesh component missing");
        const auto& ground_mesh = w.registry.get<MeshComponent>(ground).m_mesh;
        if (!ground_mesh) return scene_fail("ground mesh missing");
        if (ground_mesh->collision_cache_path.empty()) return scene_fail("ground collision cache path missing");
        if (fs::path(ground_mesh->collision_cache_path).lexically_normal() != cooked_cache.lexically_normal())
            return scene_fail("ground cache path mismatch: " + ground_mesh->collision_cache_path + " expected " + cooked_cache.string());
        JPH::ShapeRefC verified_cooked_shape;
        std::string cache_error;
        if (!Assets::CookedCollisionSerializer::loadShape(
                ground_mesh->collision_cache_path, ground_mesh->collision_source_hash,
                ground_mesh->collision_source_file_size, verified_cooked_shape, &cache_error) ||
            !verified_cooked_shape)
            return scene_fail("cooked shape load failed: " + cache_error);

        const auto& layers = w.getPhysicsSystem().layerSettings();
        if (legacy_mapping ? layers.character_body != layers.dynamic_body
                           : layers.character_body == layers.dynamic_body)
            return scene_fail("character layer configuration mismatch");

        const entt::entity character = w.registry.create();
        w.registry.emplace<TransformComponent>(character, 5.0f, 1.2f, 1.0f);
        w.registry.emplace<TagComponent>(character, "d6_t1_character");
        auto& controller = w.registry.emplace<CharacterControllerComponent>(character);
        controller.move_speed = 4.0f;
        if (w.getPhysicsSystem().createCharacterController(w.registry, character).IsInvalid())
            return scene_fail("character creation failed");

        for (int tick = 0; tick < 120; ++tick)
        {
            CharacterMoveInput input;
            if (tick < 80) input.move_forward = 1.0f;
            CharacterControllerState state;
            if (!character_physics.simulateController(PhysicsTestEcsBridge::entityRef(w, character), input, w.fixed_delta, state))
                return scene_fail("character simulation failed at tick " + std::to_string(tick));
            w.getPhysicsSystem().stepPhysics(w.registry);
            if (!character_physics.refreshController(PhysicsTestEcsBridge::entityRef(w, character)))
                return scene_fail("character refresh failed at tick " + std::to_string(tick));
        }

        auto view = w.registry.view<TagComponent, TransformComponent>();
        for (entt::entity entity : view)
        {
            if (!w.getPhysicsSystem().hasBody(entity)) continue;
            body_positions.push_back({view.get<TagComponent>(entity).name,
                                      view.get<TransformComponent>(entity).position});
        }
        std::sort(body_positions.begin(), body_positions.end(),
            [](const NamedPosition& a, const NamedPosition& b) { return a.name < b.name; });
        character_position = w.registry.get<TransformComponent>(character).position;
        if (!character_physics.readControllerState(PhysicsTestEcsBridge::entityRef(w, character), character_state))
            return scene_fail("character state read failed");
        if (body_positions.size() < 4)
        {
            std::string details;
            for (entt::entity entity : level_entities)
            {
                if (w.registry.all_of<TagComponent>(entity))
                    details += " " + w.registry.get<TagComponent>(entity).name +
                        (w.getPhysicsSystem().hasBody(entity) ? ":body" : ":no-body") +
                        (w.registry.all_of<TransformComponent>(entity) ? ":transform" : ":no-transform");
            }
            return scene_fail("expected at least four bodies, got " + std::to_string(body_positions.size()) + details);
        }
        return true;
    };

    if (!run_scene(true, legacy_body_positions, legacy_character_position, legacy_character_state))
        return fail(name, "legacy DynamicProp mapping: " + scene_error);
    std::vector<char> cooked_after_legacy;
    if (!read_bytes(cooked_cache, cooked_after_legacy) || cooked_after_legacy != cooked_before)
        return fail(name, "legacy-mapping run changed the cooked ground cache bytes");

    if (!run_scene(false, current_body_positions, current_character_position, current_character_state))
        return fail(name, "dedicated Character mapping: " + scene_error);
    std::vector<char> cooked_after_current;
    if (!read_bytes(cooked_cache, cooked_after_current) || cooked_after_current != cooked_before)
        return fail(name, "dedicated-Character run changed the cooked ground cache bytes");
    std::vector<char> original_cache_after;
    if (!read_bytes(original_cache, original_cache_after) || original_cache_after != original_cache_before)
        return fail(name, "fixture runs changed the original cooked ground cache bytes");

    auto same_vec3_bits = [](const glm::vec3& a, const glm::vec3& b)
    {
        return std::bit_cast<uint32_t>(a.x) == std::bit_cast<uint32_t>(b.x) &&
               std::bit_cast<uint32_t>(a.y) == std::bit_cast<uint32_t>(b.y) &&
               std::bit_cast<uint32_t>(a.z) == std::bit_cast<uint32_t>(b.z);
    };
    if (legacy_body_positions.size() != current_body_positions.size())
        return fail(name, "physics body set changed between layer mappings");
    for (size_t i = 0; i < legacy_body_positions.size(); ++i)
    {
        if (legacy_body_positions[i].name != current_body_positions[i].name ||
            !same_vec3_bits(legacy_body_positions[i].position, current_body_positions[i].position))
            return fail(name, "body positions differ after 120 steps at '" + legacy_body_positions[i].name + "'");
    }
    if (!same_vec3_bits(legacy_character_position, current_character_position) ||
        !same_vec3_bits(legacy_character_state.position, current_character_state.position) ||
        !same_vec3_bits(legacy_character_state.velocity, current_character_state.velocity))
        return fail(name, "character position or velocity differs after 120 steps");

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

static bool testVRTemplateReferences(const fs::path& repo_root)
{
    const std::string name = "VR template references";
    const fs::path template_root = repo_root / "Templates" / "VR";
    Assets::AssetManager::get().setAssetRoot((template_root / "assets").string());
    Assets::AssetManager::get().setAssetPrefix("assets");

    if (!fs::exists(template_root / "VR.garden"))
        return fail(name, "missing VR.garden");
    if (!fs::exists(template_root / "VR.buildscript"))
        return fail(name, "missing VR.buildscript");

    LevelManager level_manager;
    LevelData level;
    fs::path level_path = template_root / "assets" / "levels" / "main.level.json";
    if (!level_manager.loadLevel(level_path.string(), level))
        return fail(name, "failed to parse main.level.json");

    bool saw_player = false;
    bool saw_floor_collider = false;
    bool saw_walkaround_block = false;

    for (const LevelEntity& entity : level.entities)
    {
        if (!entity.mesh_path.empty())
        {
            const std::string resolved = Assets::AssetManager::get().resolveAssetPath(entity.mesh_path);
            if (!fs::exists(resolved))
                return fail(name, "missing mesh asset: " + entity.mesh_path);
        }

        if (entity.type == EntityType::Player)
        {
            saw_player = true;
            if (entity.speed < 2.0f)
                return fail(name, "player walk speed is too low for the walkaround template");
        }

        if (entity.name == "floor" && entity.has_collider && entity.collider_shape_type == "Box")
            saw_floor_collider = true;
        if (entity.name.find("walkaround_block") == 0 && entity.has_collider)
            saw_walkaround_block = true;
    }

    if (!saw_player)
        return fail(name, "template has no Player entity");
    if (!saw_floor_collider)
        return fail(name, "template floor has no box collider");
    if (!saw_walkaround_block)
        return fail(name, "template has no collidable walkaround block");

    std::ifstream module(template_root / "src" / "GameModule.cpp");
    if (!module)
        return fail(name, "missing GameModule.cpp");
    const std::string source((std::istreambuf_iterator<char>(module)),
                             std::istreambuf_iterator<char>());
    if (source.find("XR/OpenXRSystem.hpp") == std::string::npos)
        return fail(name, "game module does not touch the OpenXR runtime bridge");
    if (source.find("simulateController") == std::string::npos)
        return fail(name, "game module does not drive character-controller locomotion");

    return pass(name);
}

static bool roundTripHBPersistence(LevelManager& manager,
                                   const LevelData& source,
                                   bool binary,
                                   LevelData& loaded,
                                   std::string& error)
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path path = fs::temp_directory_path() /
        ("garden_hb_" + std::to_string(stamp) + (binary ? ".gardenbin" : ".json"));
    const bool saved = binary
        ? manager.saveLevelToBinary(path.string(), source)
        : manager.saveLevelToJSON(path.string(), source);
    if (!saved)
    {
        error = "failed to save " + std::string(binary ? "binary" : "JSON") + " fixture";
        std::error_code ec;
        fs::remove(path, ec);
        return false;
    }
    const bool loaded_ok = binary
        ? manager.loadLevelFromBinary(path.string(), loaded)
        : manager.loadLevelFromJSON(path.string(), loaded);
    std::error_code ec;
    fs::remove(path, ec);
    if (!loaded_ok)
    {
        error = "failed to reload " + std::string(binary ? "binary" : "JSON") + " fixture";
        return false;
    }
    return true;
}

static entt::entity findEntityByTag(entt::registry& registry, const std::string& name)
{
    for (auto [entity, tag] : registry.view<TagComponent>().each())
        if (tag.name == name)
            return entity;
    return entt::null;
}

// H-E portion 2 loaded-level boundary: body construction originally happened
// before H-B restored ParentComponent links. This fixture observes Jolt before
// the first physics step, so a later regular kinematic sync cannot mask a
// body initialized from the child's local pose instead of its composed world
// pose. The off-centre probe also distinguishes the parent-composed yaw from
// an unrotated box.
static bool testHELoadedKinematicChildInitializesAtComposedPose()
{
    using json = nlohmann::json;
    const std::string name = "H-E loaded kinematic child initializes from restored world pose";
    const EntityGuid parent_guid{0xE100, 0xE101};
    const EntityGuid child_guid{0xE200, 0xE201};

    LevelData level;
    level.metadata.level_name = "H-E loaded hierarchy physics fixture";
    LevelEntity child;
    child.name = "he_loaded_kinematic_child";
    child.type = EntityType::Physical;
    child.position = {1.0f, 0.25f, 0.0f};
    child.rotation = {0.0f, 0.0f, 0.0f};
    child.scale = glm::vec3(1.0f);
    child.has_rigidbody = true;
    child.body_motion_type = "Kinematic";
    child.apply_gravity = false;
    child.has_collider = true;
    child.collider_shape_type = "Box";
    child.collider_box_half_extents = {2.0f, 0.25f, 0.5f};
    child.reflected_components = json::object({
        {"IdentityComponent", json::object({{"guid", guidToString(child_guid)}})},
        {"ParentComponent", json::object({{"parent", guidToString(parent_guid)}})}
    });

    LevelEntity parent;
    parent.name = "he_loaded_parent";
    parent.type = EntityType::Static;
    parent.position = {10.0f, 0.0f, 0.0f};
    parent.rotation = {0.0f, 90.0f, 0.0f};
    parent.scale = glm::vec3(1.0f);
    parent.reflected_components = json::object({
        {"IdentityComponent", json::object({{"guid", guidToString(parent_guid)}})}
    });

    // Child first also confirms the post-link initialization does not depend
    // on authored file order.
    level.entities.push_back(child);
    level.entities.push_back(parent);

    world w;
    w.initializePhysics();
    EntityLifecycle lifecycle(w.registry, 72);
    LevelManager manager;
    std::vector<entt::entity> created;
    if (!manager.instantiateLevel(level, w, nullptr, nullptr, nullptr,
                                  nullptr, false, &created))
        return fail(name, "valid parented kinematic level was rejected");

    const entt::entity loaded_parent = findEntityByTag(w.registry, "he_loaded_parent");
    const entt::entity loaded_child = findEntityByTag(w.registry, "he_loaded_kinematic_child");
    if (loaded_parent == entt::null || loaded_child == entt::null ||
        !w.getPhysicsSystem().hasBody(loaded_child))
        return fail(name, "loaded hierarchy or child Jolt body was missing");
    if (Hierarchy::parentOf(w.registry, loaded_child) != loaded_parent)
        return fail(name, "loaded child parent link was not restored");

    const TransformComponent& loaded_local =
        w.registry.get<TransformComponent>(loaded_child);
    if (glm::distance(loaded_local.position, child.position) > 0.0001f ||
        glm::distance(loaded_local.rotation, child.rotation) > 0.0001f ||
        glm::distance(loaded_local.scale, child.scale) > 0.0001f)
        return fail(name, "loaded body initialization overwrote child local transform");

    const glm::mat4 expected_world =
        Hierarchy::computeWorldMatrix(w.registry, loaded_child);
    const glm::vec3 expected_position(expected_world[3]);
    PhysicsSystem::OverlapResult results[4];
    const PhysicsSystem::QueryFilter filter{};
    const auto includes_child = [&](const glm::vec3& center)
    {
        const uint32_t count = w.getPhysicsSystem().overlapBox(
            center, glm::vec3(0.04f), glm::vec3(0.0f), filter, results, 4);
        for (uint32_t i = 0; i < count; ++i)
            if (results[i].entity == loaded_child)
                return true;
        return false;
    };
    if (!includes_child(expected_position))
        return fail(name, "child Jolt body was not initialized at composed parent translation");
    if (!includes_child(expected_position + glm::vec3(0.0f, 0.0f, 1.25f)))
        return fail(name, "child Jolt body was not initialized with composed parent rotation");

    return pass(name);
}

// The staged load policy must classify a use_mesh_collision authoring request
// the same way that the direct API classifies its resulting mesh collider:
// as a static mesh body that cannot be a hierarchy child. This also keeps the
// unit-scale constraint scoped to actual kinematic bodies, so ordinary ECS
// children remain usable below scaled presentation parents.
static bool testHEStagedMeshPolicyAndNonPhysicsScale()
{
    using json = nlohmann::json;
    const std::string name = "H-E staged mesh policy matches direct attach and allows non-physics scaled children";

    // Direct hierarchy API: a child with no physics component is allowed below
    // a scaled parent. The scale invariant applies only to a kinematic body.
    {
        entt::registry registry;
        EntityLifecycle lifecycle(registry, 73);
        const entt::entity parent = lifecycle.create();
        const entt::entity child = lifecycle.create();
        registry.emplace<TransformComponent>(parent).scale = {2.0f, 1.0f, 1.0f};
        registry.emplace<TransformComponent>(child);
        if (!Hierarchy::attach(registry, child, parent))
            return fail(name, "direct attach rejected a non-physics child below a scaled parent");
        if (Hierarchy::parentOf(registry, child) != parent)
            return fail(name, "direct non-physics child link was not written");
    }

    const EntityGuid parent_guid{0xE300, 0xE301};
    const EntityGuid child_guid{0xE400, 0xE401};
    const auto make_parent = [&]()
    {
        LevelEntity parent;
        parent.name = "he_policy_parent";
        parent.scale = {2.0f, 1.0f, 1.0f};
        parent.reflected_components = json::object({
            {"IdentityComponent", json::object({{"guid", guidToString(parent_guid)}})}
        });
        return parent;
    };
    const auto make_child = [&](const char* child_name)
    {
        LevelEntity child;
        child.name = child_name;
        child.reflected_components = json::object({
            {"IdentityComponent", json::object({{"guid", guidToString(child_guid)}})},
            {"ParentComponent", json::object({{"parent", guidToString(parent_guid)}})}
        });
        return child;
    };

    // The staged version has the same non-physics result and restores its
    // link, confirming the preflight has not made ancestor scale global.
    {
        LevelData level;
        level.metadata.level_name = "H-E non-physics scaled hierarchy fixture";
        level.entities.push_back(make_parent());
        level.entities.push_back(make_child("he_nonphysics_child"));
        world game_world;
        LevelManager manager;
        std::vector<entt::entity> created;
        if (!manager.instantiateLevel(level, game_world, nullptr, nullptr,
                                      nullptr, nullptr, false, &created))
            return fail(name, "staged load rejected a non-physics child below a scaled parent");
        if (created.size() != 2 ||
            Hierarchy::parentOf(game_world.registry, created[1]) != created[0])
            return fail(name, "staged non-physics child link was not restored");
    }

    // A Physical kinematic child which requests visual mesh collision is
    // destined to receive a Mesh ColliderComponent and static mesh body. The
    // staged preflight must reject before it creates any registry entity or
    // can create a Jolt body.
    {
        LevelData level;
        level.metadata.level_name = "H-E staged kinematic mesh rejection fixture";
        level.entities.push_back(make_parent());
        LevelEntity child = make_child("he_kinematic_mesh_child");
        child.type = EntityType::Physical;
        child.body_motion_type = "Kinematic";
        child.use_mesh_collision = true;
        child.mesh_path = "fixtures/he_kinematic_mesh_collision.obj";
        level.entities.push_back(child);

        world game_world;
        game_world.initializePhysics();
        const entt::entity existing = game_world.registry.create();
        game_world.registry.emplace<TagComponent>(existing, TagComponent{"existing"});
        const size_t tag_count_before = game_world.registry.view<TagComponent>().size();
        const size_t identity_count_before = game_world.registry.view<IdentityComponent>().size();
        LevelManager manager;
        std::vector<entt::entity> created;
        if (manager.instantiateLevel(level, game_world, nullptr, nullptr,
                                     nullptr, nullptr, false, &created))
            return fail(name, "staged kinematic mesh child was accepted");
        if (!created.empty() ||
            game_world.registry.view<TagComponent>().size() != tag_count_before ||
            game_world.registry.view<IdentityComponent>().size() != identity_count_before)
            return fail(name, "staged mesh rejection mutated the registry before body creation");
    }

    return pass(name);
}

static LevelData makeHBCohortFixture()
{
    using json = nlohmann::json;
    LevelData level;
    level.metadata.level_name = "H-B cohort fixture";
    const EntityGuid root_guid{0xA100, 0xB100};
    const EntityGuid first_guid{0xA200, 0xB200};
    const EntityGuid external_guid{0xDEAD, 0xBEEF};
    const EntityGuid fake_child_guid{0xFFFF, 0xFFFF};
    const EntityGuid fake_mirror_guid{0xEEEE, 0xEEEE};

    LevelEntity root;
    root.name = "hb_root";
    root.reflected_components = json::object({
        {"IdentityComponent", json::object({{"guid", guidToString(root_guid)}})},
        {"ChildrenComponent", json::object({{"children", json::array({guidToString(fake_child_guid)})}})}
    });

    LevelEntity first_child;
    first_child.name = "hb_first_child";
    first_child.reflected_components = json::object({
        {"IdentityComponent", json::object({{"guid", guidToString(first_guid)}})},
        {"ParentComponent", json::object({{"parent", guidToString(root_guid)}})},
        {"ChildrenComponent", json::object({{"children", json::array({guidToString(fake_mirror_guid)})}})}
    });
    level.entities.push_back(first_child);
    // The child intentionally precedes its parent in file order. Restore must
    // wait until every cohort identity exists before resolving this link.
    level.entities.push_back(root);

    // A legacy entity without an IdentityComponent receives a generated ID.
    LevelEntity missing_identity;
    missing_identity.name = "hb_missing_identity";
    missing_identity.reflected_components = json::object({
        {"ParentComponent", json::object({{"parent", guidToString(root_guid)}})}
    });
    level.entities.push_back(missing_identity);

    // A legacy serialized null identity also receives a generated ID.
    LevelEntity null_identity;
    null_identity.name = "hb_null_identity";
    null_identity.reflected_components = json::object({
        {"IdentityComponent", json::object({{"guid", guidToString(EntityGuid{})}})},
        {"ParentComponent", json::object({{"parent", guidToString(root_guid)}})}
    });
    level.entities.push_back(null_identity);

    // This destination-registry target is external to the cohort and must not
    // be adopted as a parent during saved-level restore.
    LevelEntity external_parent_child;
    external_parent_child.name = "hb_external_parent_child";
    external_parent_child.reflected_components = json::object({
        {"ParentComponent", json::object({{"parent", guidToString(external_guid)}})}
    });
    level.entities.push_back(external_parent_child);

    // A valid but absent GUID exercises the missing-parent policy separately
    // from the existing destination-registry external-parent case.
    LevelEntity missing_parent_child;
    missing_parent_child.name = "hb_missing_parent_child";
    missing_parent_child.reflected_components = json::object({
        {"ParentComponent", json::object({{"parent", guidToString(EntityGuid{0xBAD, 0xBAD})}})}
    });
    level.entities.push_back(missing_parent_child);

    LevelEntity null_component;
    null_component.name = "hb_null_parent_component";
    null_component.reflected_components = json::object({{"ParentComponent", nullptr}});
    level.entities.push_back(null_component);

    LevelEntity null_reference;
    null_reference.name = "hb_null_parent_reference";
    null_reference.reflected_components = json::object({
        {"ParentComponent", json::object({{"parent", guidToString(EntityGuid{})}})}
    });
    level.entities.push_back(null_reference);

    LevelEntity player;
    player.name = "hb_player";
    player.type = EntityType::Player;
    player.reflected_components = json::object({
        {"IdentityComponent", json::object({{"guid", guidToString(EntityGuid{0xA300, 0xB300})}})},
        {"PlayerComponent", json::object({{"speed", 4.5f}})}
    });
    level.entities.push_back(player);
    return level;
}

static bool testLevelManagerHBPersistenceAndCohortRestore()
{
    using json = nlohmann::json;
    const std::string name = "LevelManager H-B JSON/binary identity and hierarchy restore";
    for (bool binary : {false, true})
    {
        LevelManager manager;
        ReflectionRegistry reflection;
        registerEngineReflection(reflection);
        manager.setReflectionRegistry(&reflection);

        LevelData loaded;
        std::string error;
        if (!roundTripHBPersistence(manager, makeHBCohortFixture(), binary,
                                   loaded, error))
            return fail(name, error);

        world game_world;
        const EntityGuid outside_guid{0xDEAD, 0xBEEF};
        const EntityRef outside_mirror_ref{EntityGuid{0xCAFE, 0xCAFE}};
        const entt::entity outside_parent = game_world.registry.create();
        game_world.registry.emplace<IdentityComponent>(outside_parent, outside_guid);
        game_world.registry.emplace<TagComponent>(outside_parent, TagComponent{"outside_parent"});
        game_world.registry.emplace<ChildrenComponent>(
            outside_parent, ChildrenComponent{{outside_mirror_ref}});
        const auto saved_outside_mirror =
            game_world.registry.get<ChildrenComponent>(outside_parent).children;
        EntityLifecycle lifecycle(game_world.registry, 83);

        std::vector<entt::entity> created;
        if (!manager.instantiateLevel(loaded, game_world, nullptr, nullptr,
                                      nullptr, nullptr, false, &created))
            return fail(name, "valid persisted cohort was rejected");
        if (created.size() != loaded.entities.size())
            return fail(name, "created entity count did not match LevelData order");

        const entt::entity root = findEntityByTag(game_world.registry, "hb_root");
        const entt::entity first = findEntityByTag(game_world.registry, "hb_first_child");
        const entt::entity missing = findEntityByTag(game_world.registry, "hb_missing_identity");
        const entt::entity null_id = findEntityByTag(game_world.registry, "hb_null_identity");
        const entt::entity external_child = findEntityByTag(game_world.registry, "hb_external_parent_child");
        const entt::entity missing_parent_child = findEntityByTag(game_world.registry, "hb_missing_parent_child");
        const entt::entity null_component = findEntityByTag(game_world.registry, "hb_null_parent_component");
        const entt::entity null_reference = findEntityByTag(game_world.registry, "hb_null_parent_reference");
        const entt::entity player = findEntityByTag(game_world.registry, "hb_player");
        if (root == entt::null || first == entt::null || missing == entt::null ||
            null_id == entt::null || external_child == entt::null ||
            missing_parent_child == entt::null ||
            null_component == entt::null || null_reference == entt::null || player == entt::null)
            return fail(name, "one of the fixture entities was not instantiated");

        const EntityGuid root_guid{0xA100, 0xB100};
        const EntityGuid first_guid{0xA200, 0xB200};
        const EntityGuid player_guid{0xA300, 0xB300};
        if (game_world.registry.get<IdentityComponent>(root).guid != root_guid ||
            game_world.registry.get<IdentityComponent>(first).guid != first_guid ||
            game_world.registry.get<IdentityComponent>(player).guid != player_guid)
            return fail(name, "valid serialized identity GUID was not preserved");

        std::unordered_set<EntityGuid, GuidHash> unique_ids;
        for (entt::entity entity : created)
        {
            if (!game_world.registry.all_of<IdentityComponent>(entity))
                return fail(name, "loaded entity has no IdentityComponent");
            const EntityGuid guid = game_world.registry.get<IdentityComponent>(entity).guid;
            if (guid.isNull() || !unique_ids.insert(guid).second)
                return fail(name, "loaded entity identity is null or duplicated");
            if (lifecycle.resolve(EntityRef{guid}) != entity)
                return fail(name, "lifecycle construct hook did not index the final identity");
        }

        // Query directly: scopedEntity(ref) would lazily repair a missing marker.
        std::unordered_set<EntityGuid, GuidHash> queried_ids;
        bool invalid_query_identity = false;
        const auto query_result = game_world.query<const TransformComponent>(
            [&](garden::ecs::EntityHandle handle, const TransformComponent&) {
                const EntityRef reference = game_world.persistedReference(handle);
                if (reference.isNull() || !queried_ids.insert(reference.guid).second)
                    invalid_query_identity = true;
            });
        if (query_result != garden::ecs::ComponentStoreResult::Success ||
            invalid_query_identity || queried_ids != unique_ids)
            return fail(name, "Garden transform query did not expose exactly the loaded identities");

        std::unordered_set<EntityGuid, GuidHash> queried_players;
        const auto player_query_result = game_world.query<const PlayerComponent, const TransformComponent>(
            [&](garden::ecs::EntityHandle handle, const PlayerComponent&, const TransformComponent&) {
                queried_players.insert(game_world.persistedReference(handle).guid);
            });
        if (player_query_result != garden::ecs::ComponentStoreResult::Success ||
            queried_players.size() != 1 || queried_players.count(player_guid) != 1)
            return fail(name, "Garden player/transform query did not expose the saved player GUID");

        const EntityGuid missing_guid = game_world.registry.get<IdentityComponent>(missing).guid;
        const EntityGuid null_guid = game_world.registry.get<IdentityComponent>(null_id).guid;
        if (missing_guid == null_guid || missing_guid.isNull() || null_guid.isNull())
            return fail(name, "legacy missing/null identities were not assigned distinct GUIDs");
        const json saved_missing = ReflectionSerializer::serializeEntity(
            game_world.registry, missing, reflection);
        const std::string reflected_missing_guid =
            saved_missing["components"]["IdentityComponent"]["guid"].get<std::string>();
        if (reflected_missing_guid != guidToString(missing_guid))
            return fail(name, "generated legacy GUID is not present in reflected save data");

        // Model EditorApp's save path: reflected live components are copied
        // into LevelEntity, then the level format persists that representation.
        LevelEntity editor_saved_entity;
        editor_saved_entity.name = "hb_missing_identity";
        editor_saved_entity.reflected_components = saved_missing["components"];
        LevelData editor_saved_level;
        editor_saved_level.metadata.level_name = "H-B editor save fixture";
        editor_saved_level.entities.push_back(editor_saved_entity);
        for (bool saved_as_binary : {false, true})
        {
            LevelData editor_reloaded_level;
            if (!roundTripHBPersistence(manager, editor_saved_level, saved_as_binary,
                                        editor_reloaded_level, error))
                return fail(name, error);
            if (editor_reloaded_level.entities.size() != 1 ||
                editor_reloaded_level.entities[0].reflected_components
                    ["IdentityComponent"]["guid"].get<std::string>() != reflected_missing_guid)
                return fail(name, "generated GUID did not persist through editor-style JSON/binary save");
        }

        const auto* root_children = game_world.registry.try_get<ChildrenComponent>(root);
        if (!root_children || root_children->children.size() != 3 ||
            root_children->children[0] != EntityRef{first_guid} ||
            root_children->children[1] != EntityRef{missing_guid} ||
            root_children->children[2] != EntityRef{null_guid})
            return fail(name, "Children mirror did not rebuild in LevelData file order");
        if (!game_world.registry.all_of<ParentComponent>(first) ||
            game_world.registry.get<ParentComponent>(first).parent != EntityRef{root_guid})
            return fail(name, "valid in-cohort ParentComponent was not restored");
        if (game_world.registry.all_of<ParentComponent>(external_child) ||
            game_world.registry.all_of<ParentComponent>(missing_parent_child) ||
            game_world.registry.all_of<ParentComponent>(null_component) ||
            game_world.registry.all_of<ParentComponent>(null_reference))
            return fail(name, "external/null parent data was not removed");
        if (game_world.registry.get<ChildrenComponent>(outside_parent).children !=
            saved_outside_mirror)
            return fail(name, "cohort rebuild changed an unrelated registry mirror");
    }
    return pass(name);
}

static LevelData makeHBRejectedFixture(const std::string& scenario)
{
    using json = nlohmann::json;
    LevelData level;
    level.metadata.level_name = "H-B rejected fixture";
    const EntityGuid first_guid{0x7100, 0x8100};
    const EntityGuid second_guid{0x7200, 0x8200};
    LevelEntity first;
    first.name = "hb_rejected_first";
    first.reflected_components = json::object({
        {"IdentityComponent", json::object({{"guid", guidToString(first_guid)}})}
    });

    if (scenario == "malformed_identity")
        first.reflected_components["IdentityComponent"]["guid"] = "not-a-guid";
    else if (scenario == "malformed_parent")
        first.reflected_components["ParentComponent"] =
            json::object({{"parent", "not-a-guid"}});
    else if (scenario == "self_link")
        first.reflected_components["ParentComponent"] =
            json::object({{"parent", guidToString(first_guid)}});

    level.entities.push_back(first);
    if (scenario == "duplicate")
    {
        LevelEntity second;
        second.name = "hb_rejected_duplicate";
        second.reflected_components = json::object({
            {"IdentityComponent", json::object({{"guid", guidToString(first_guid)}})}
        });
        level.entities.push_back(second);
    }
    else if (scenario == "cycle")
    {
        level.entities[0].reflected_components["ParentComponent"] =
            json::object({{"parent", guidToString(second_guid)}});
        LevelEntity second;
        second.name = "hb_rejected_cycle";
        second.reflected_components = json::object({
            {"IdentityComponent", json::object({{"guid", guidToString(second_guid)}})},
            {"ParentComponent", json::object({{"parent", guidToString(first_guid)}})}
        });
        level.entities.push_back(second);
    }
    return level;
}

static bool testLevelManagerHBRejectsBeforeRegistryMutation()
{
    const std::string name = "LevelManager H-B rejects malformed cohorts before registry mutation";
    const std::vector<std::string> scenarios = {
        "malformed_identity", "malformed_parent", "duplicate", "collision",
        "self_link", "cycle"
    };
    for (bool binary : {false, true})
    {
        for (const std::string& scenario : scenarios)
        {
            LevelManager manager;
            ReflectionRegistry reflection;
            registerEngineReflection(reflection);
            manager.setReflectionRegistry(&reflection);
            LevelData loaded;
            std::string error;
            if (!roundTripHBPersistence(manager, makeHBRejectedFixture(scenario),
                                        binary, loaded, error))
                return fail(name, error);

            world game_world;
            const EntityGuid existing_guid = scenario == "collision"
                ? EntityGuid{0x7100, 0x8100}
                : EntityGuid{0x9100, 0xA100};
            const EntityRef existing_child_ref{EntityGuid{0xB100, 0xC100}};
            const entt::entity existing = game_world.registry.create();
            game_world.registry.emplace<IdentityComponent>(existing, existing_guid);
            game_world.registry.emplace<TagComponent>(existing, TagComponent{"existing"});
            game_world.registry.emplace<ChildrenComponent>(
                existing, ChildrenComponent{{existing_child_ref}});
            const size_t tag_count_before =
                game_world.registry.view<TagComponent>().size();
            const auto outside_mirror_before =
                game_world.registry.get<ChildrenComponent>(existing).children;
            EntityLifecycle lifecycle(game_world.registry, 84);
            const EntityRef scoped_ref{EntityGuid{0xDD00, 0xEE00}};
            const auto scoped = game_world.restoreScopedEntity(scoped_ref);
            if (!game_world.isEntityValid(scoped) ||
                game_world.add<TransformComponent>(scoped) != garden::ecs::ComponentStoreResult::Success)
                return fail(name, "could not create existing scoped query sentinel");
            const auto query_scoped = [&]() {
                std::unordered_set<EntityGuid, GuidHash> ids;
                const auto result = game_world.query<const TransformComponent>(
                    [&](garden::ecs::EntityHandle handle, const TransformComponent&) {
                        ids.insert(game_world.persistedReference(handle).guid);
                    });
                return std::make_pair(result, ids);
            };
            const auto scoped_before = query_scoped();
            if (scoped_before.first != garden::ecs::ComponentStoreResult::Success ||
                scoped_before.second.size() != 1 || scoped_before.second.count(scoped_ref.guid) != 1)
                return fail(name, "existing scoped query sentinel was not visible");
            // Include the sentinel in the destination counts checked after rejection.
            const size_t scoped_identity_count_before =
                game_world.registry.view<IdentityComponent>().size();
            std::vector<entt::entity> created;
            if (manager.instantiateLevel(loaded, game_world, nullptr, nullptr,
                                         nullptr, nullptr, false, &created))
                return fail(name, scenario + " cohort was unexpectedly accepted");
            if (!created.empty() ||
                game_world.registry.view<IdentityComponent>().size() != scoped_identity_count_before ||
                game_world.registry.view<TagComponent>().size() != tag_count_before)
                return fail(name, scenario + " rejection mutated the destination registry");
            if (game_world.registry.get<ChildrenComponent>(existing).children !=
                outside_mirror_before)
                return fail(name, scenario + " rejection changed an existing mirror");
            if (query_scoped() != scoped_before)
                return fail(name, scenario + " rejection changed Garden-queryable entities");
        }
    }
    return pass(name);
}

static void failSecondLevelIdentity(entt::registry& registry, entt::entity entity)
{
    if (registry.get<IdentityComponent>(entity).guid == EntityGuid{0x7200, 0x8200})
        throw 1;
}

static bool testLevelManagerLargeIdentityCohort()
{
    const std::string name = "LevelManager restores a large identity cohort";
    LevelManager manager;
    world w;
    const EntityRef sentinel_ref{EntityGuid{0xCC00, 0xDD00}};
    const auto sentinel = w.restoreScopedEntity(sentinel_ref);
    LevelData level;
    constexpr size_t count = 2048;
    for (size_t i = 0; i < count; ++i)
    {
        LevelEntity entity;
        entity.reflected_components = nlohmann::json::object({
            {"IdentityComponent", {{"guid", guidToString(EntityGuid{0xF000u + i, 0xA000u + i})}}}
        });
        level.entities.push_back(std::move(entity));
    }
    std::vector<entt::entity> created;
    if (!manager.instantiateLevel(level, w, nullptr, nullptr, nullptr, nullptr, false, &created) ||
        created.size() != count || w.persistedReference(sentinel) != sentinel_ref)
        return fail(name, "cohort restore failed or changed sentinel");
    for (size_t i = 0; i < count; ++i)
        if (w.registry.get<IdentityComponent>(created[i]).guid != EntityGuid{0xF000u + i, 0xA000u + i})
            return fail(name, "cohort order or identity changed");
    return pass(name);
}

static std::vector<entt::entity> levelRollbackObserved;
static bool levelRollbackThrowComponent = false;
static void observeLevelRollbackTransform(entt::registry&, entt::entity entity)
{
    levelRollbackObserved.push_back(entity);
    if (levelRollbackThrowComponent && levelRollbackObserved.size() == 2)
        throw 1;
}

static bool testLevelManagerPostCreationRollback()
{
    const std::string name = "LevelManager rolls back component and physics failures";
    for (bool component_failure : {true, false})
    {
        LevelManager manager;
        world w;
        const EntityRef sentinel_ref{EntityGuid{0xDD01, 0xEE01}};
        const auto sentinel = w.restoreScopedEntity(sentinel_ref);
        const auto old_gravity = w.getGravity();
        const auto old_delta = w.getFixedDelta();
        LevelData level;
        level.metadata.gravity = old_gravity + glm::vec3(1, 2, 3);
        level.metadata.fixed_delta = old_delta * 2;
        for (int i = 0; i != 2; ++i)
        {
            LevelEntity entity;
            entity.type = EntityType::Physical;
            entity.has_collider = true;
            entity.collider_shape_type = i == 0 ? "Box" : "Mesh";
            entity.reflected_components = nlohmann::json::object({
                {"IdentityComponent", {{"guid", guidToString(EntityGuid{0xA100u + i, 0xB100u + i})}}}
            });
            level.entities.push_back(entity);
        }
        levelRollbackObserved.clear();
        levelRollbackThrowComponent = component_failure;
        auto connection = w.registry.on_construct<TransformComponent>().connect<&observeLevelRollbackTransform>();
        entt::entity player = entt::entity{123};
        std::vector<entt::entity> created{entt::entity{456}};
        const bool success = manager.instantiateLevel(level, w, nullptr, &player, nullptr, nullptr, false, &created);
        connection.release();
        levelRollbackThrowComponent = false;
        if (success || levelRollbackObserved.size() != 2 || player != entt::entity{123} ||
            created != std::vector<entt::entity>{entt::entity{456}} ||
            w.getGravity() != old_gravity || w.getFixedDelta() != old_delta ||
            w.persistedReference(sentinel) != sentinel_ref ||
            w.registry.view<IdentityComponent>().size() != 1)
            return fail(name, "failure changed sentinel, settings, or outputs");
        for (auto e : levelRollbackObserved)
            if (w.registry.valid(e) || w.getPhysicsSystem().hasBody(e) ||
                w.getPhysicsSystem().hasCharacterController(e))
                return fail(name, "failed cohort retained entity or physics state");
    }
    return pass(name);
}

static bool testLevelManagerReflectedFailureRollback()
{
    const std::string name = "LevelManager reflected failure rolls back cohort";
    for (int fault = 0; fault != 3; ++fault)
    {
        LevelManager manager;
        ReflectionRegistry reflection;
        registerEngineReflection(reflection);
        if (fault == 2) {
            auto descriptor = *reflection.findByName("PlayerComponent");
            descriptor.add = [](entt::registry&, entt::entity) { throw std::runtime_error("injected component construction"); };
            reflection.registerComponent(std::move(descriptor));
        }
        manager.setReflectionRegistry(&reflection);
        world w;
        const EntityRef sentinel_ref{EntityGuid{0xAA11, 0xBB11}};
        auto sentinel = w.restoreScopedEntity(sentinel_ref);
        const auto old_gravity = w.getGravity();
        const auto old_delta = w.getFixedDelta();
        LevelData level;
        level.metadata.gravity = old_gravity + glm::vec3(1, 2, 3);
        level.metadata.fixed_delta = old_delta * 2;
        LevelEntity good;
        good.reflected_components = {{"PlayerComponent", {{"speed", 4.0f}}}};
        level.entities.push_back(good);
        LevelEntity bad;
        if (fault == 0) bad.reflected_components = {{"UnknownComponent", nlohmann::json::object()}};
        else if (fault == 1) bad.reflected_components = {{"PlayerComponent", {{"speed", "bad"}}}};
        else { level.entities.clear(); bad.reflected_components = {{"PlayerComponent", {{"speed", 4.0f}}}}; }
        level.entities.push_back(bad);
        entt::entity output = entt::entity{123};
        std::vector<entt::entity> created{entt::entity{456}};
        if (manager.instantiateLevel(level, w, nullptr, &output, nullptr, nullptr, false, &created) ||
            output != entt::entity{123} || created != std::vector<entt::entity>{entt::entity{456}} ||
            w.getGravity() != old_gravity || w.getFixedDelta() != old_delta ||
            w.persistedReference(sentinel) != sentinel_ref || w.registry.view<IdentityComponent>().size() != 1)
            return fail(name, "reflected failure changed world, settings or outputs");
    }
    return pass(name);
}

static bool testLevelManagerScopedRestoreRollback()
{
    const std::string name = "LevelManager rolls back failed scoped cohort restore";
    LevelManager manager;
    world game_world;
    EntityLifecycle lifecycle(game_world.registry, 85);
    const EntityRef sentinel_ref{EntityGuid{0xDD00, 0xEE00}};
    const auto sentinel = game_world.restoreScopedEntity(sentinel_ref);
    if (!game_world.isEntityValid(sentinel) ||
        game_world.add<TransformComponent>(sentinel) != garden::ecs::ComponentStoreResult::Success)
        return fail(name, "could not create existing scoped sentinel");
    const size_t identities_before = game_world.registry.view<IdentityComponent>().size();
    const float fixed_delta_before = game_world.fixed_delta;
    const glm::vec3 gravity_before = game_world.getGravity();

    LevelData level;
    level.metadata.fixed_delta = fixed_delta_before * 2.0f;
    level.metadata.gravity = gravity_before + glm::vec3(1.0f, 2.0f, 3.0f);
    for (const EntityGuid guid : {EntityGuid{0x7100, 0x8100}, EntityGuid{0x7200, 0x8200}})
    {
        LevelEntity entity;
        entity.reflected_components = nlohmann::json::object({
            {"IdentityComponent", nlohmann::json::object({{"guid", guidToString(guid)}})}
        });
        level.entities.push_back(entity);
    }
    auto connection = game_world.registry.on_construct<IdentityComponent>()
        .connect<&failSecondLevelIdentity>();
    entt::entity player_output = entt::entity{123};
    std::vector<entt::entity> created{entt::entity{456}};
    const bool success = manager.instantiateLevel(level, game_world, nullptr, &player_output,
                                                   nullptr, nullptr, false, &created);
    connection.release();
    if (success || player_output != entt::entity{123} ||
        created != std::vector<entt::entity>{entt::entity{456}} ||
        game_world.fixed_delta != fixed_delta_before ||
        game_world.getGravity() != gravity_before ||
        game_world.registry.view<IdentityComponent>().size() != identities_before ||
        lifecycle.resolve(EntityRef{EntityGuid{0x7100, 0x8100}}) != entt::null ||
        lifecycle.resolve(EntityRef{EntityGuid{0x7200, 0x8200}}) != entt::null ||
        game_world.persistedReference(sentinel) != sentinel_ref)
        return fail(name, "failed restore changed existing state or published partial output");
    size_t visits = 0;
    bool sentinel_only = true;
    const auto result = game_world.query<const TransformComponent>(
        [&](garden::ecs::EntityHandle handle, const TransformComponent&) {
            ++visits;
            sentinel_only = sentinel_only && game_world.persistedReference(handle) == sentinel_ref;
        });
    if (result != garden::ecs::ComponentStoreResult::Success || visits != 1 || !sentinel_only)
        return fail(name, "failed restore changed Garden query results");
    return pass(name);
}

int main()
{
    EE::CLog::Init();
    fs::path repo_root = findRepoRoot();
    bool ok = true;
    run("fixed tick accumulator carries partial time");
    ok = testFixedTickAccumulatorCarriesPartialTime() && ok;
    run("fixed tick accumulator caps catch-up");
    ok = testFixedTickAccumulatorCapsCatchUp() && ok;
    run("world simulation tick tracks physics steps");
    ok = testWorldSimulationTickTracksPhysicsSteps() && ok;
    run("world time tracks physics steps");
    ok = testWorldTimeTracksPhysicsSteps() && ok;
    run("frame split keeps deterministic tick and time");
    ok = testFrameSplitKeepsDeterministicTickAndTime() && ok;
    run("tick context names substeps");
    ok = testTickContextNamesSubsteps() && ok;
    run("wish move right matches view right");
    ok = testWishMoveRightMatchesViewRight() && ok;
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
    run("jump sets velocity and apex in range");
    ok = testJumpApexAndSetVelocity() && ok;
    run("friction reaches exactly zero");
    ok = testFrictionReachesExactlyZero() && ok;
    run("air accel capped first tick");
    ok = testAirAccelCappedFirstTick() && ok;
    run("sprint exceeds walk max");
    ok = testSprintExceedsWalkMax() && ok;
    run("coyote jump fires after leaving ground");
    ok = testCoyoteJump() && ok;
    run("jump buffer fires on landing");
    ok = testJumpBufferFiresOnLanding() && ok;
    run("variable jump cut lowers apex");
    ok = testVariableJumpCutLowersApex() && ok;
    run("fall speed clamped horizontal preserved");
    ok = testFallSpeedClampPreservesHorizontal() && ok;
    run("slope projected wish");
    ok = testSlopeProjectedWish() && ok;
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
    run("player controller consumes local ECS input");
    ok = testPlayerControllerUsesLocalEcsInput() && ok;
    run("player controller consumes ECS gamepad look intent");
    ok = testPlayerControllerConsumesGamepadLookIntent() && ok;
    run("network-spawned player grounds on mesh");
    ok = testNetworkSpawnedPlayerGroundsOnMesh(repo_root) && ok;
    run("CharacterController physical steps and slopes");
    ok = testCharacterControllerPhysicalStepsAndSlopes() && ok;
    run("level player creates character controller");
    ok = testLevelPlayerCreatesCharacterController() && ok;
    run("use_mesh_collision creates body");
    ok = testUseMeshCollisionCreatesBody(repo_root) && ok;
    run("D6-T6 cooked mesh regression");
    ok = testD6CookedMeshAndWaterMaterialExclusion() && ok;
    run("terrain builder heightmap mesh");
    ok = testTerrainBuilderHeightmapMesh(repo_root) && ok;
    run("level terrain creates mesh and collision");
    ok = testLevelTerrainCreatesMeshAndCollision(repo_root) && ok;
    run("raycast closest ignores shooter body");
    ok = testRaycastClosestCanIgnoreShooterBody() && ok;
    run("D6 layer matrix");
    ok = testD6LayerMatrix() && ok;
    run("D6 analytic overlap inside edge and outside");
    ok = testD6OverlapAnalyticCases() && ok;
    run("D6 query filter excludes triggers by default");
    ok = testD6QueryFilterTriggers() && ok;
    run("D6 query ignore-list raycasts");
    ok = testD6QueryIgnoreListRaycasts() && ok;
    run("D6 dynamic body teleport");
    ok = testD6DynamicBodyTeleport() && ok;
    run("D6 kinematic platform carries CharacterController");
    ok = testD6KinematicPlatformCarriesCharacter() && ok;
    run("H-E parented kinematic sync composes pose and carries CharacterVirtual");
    ok = testHEParentedKinematicPlatformSync() && ok;
    run("D6 CharacterController teleport refreshes contacts");
    ok = testD6CharacterTeleportRefreshesContacts() && ok;
    run("D6 trigger enter exit exactly once");
    ok = testD6TriggerLifecycle() && ok;
    run("D6 trigger lifecycle with CharacterController");
    ok = testD6CharacterTriggerLifecycle() && ok;
    run("D6 trigger cap and mesh rejection");
    ok = testD6TriggerCapAndMeshRejection() && ok;
    run("D6 trigger tracks more than 64 occupants");
    ok = testD6TriggerOccupantCapacity() && ok;
    run("D6-T3 sensor non-interference");
    ok = testD6SensorNonInterference() && ok;
    run("player controller consumes buffered moving jump on fixed step");
    ok = testPlayerControllerBufferedMovingJump() && ok;
    run("character physics service reports runtime controller availability");
    ok = testCharacterPhysicsServiceControllerAvailability() && ok;
    run("server/client same-build CharacterVirtual movement parity");
    ok = testServerClientCharacterMovementParity() && ok;
    run("prediction replay preserves CharacterVirtual wall collision");
    ok = testPredictionReplayPreservesCharacterCollision() && ok;
    run("D6-U1 level round-trip");
    ok = testD6LevelRoundTrip() && ok;
    run("H-E loaded kinematic child initializes from restored world pose");
    ok = testHELoadedKinematicChildInitializesAtComposedPose() && ok;
    run("H-E staged mesh policy matches direct attach and allows non-physics scaled children");
    ok = testHEStagedMeshPolicyAndNonPhysicsScale() && ok;
    run("D6-T1 legacy/current character layer comparison");
    ok = testD6LayerRenumberRegression(repo_root) && ok;
    run("FPSShooter level references");
    ok = testFpsShooterLevelReferences(repo_root) && ok;
    run("ThirdPerson template references");
    ok = testThirdPersonTemplateReferences(repo_root) && ok;
    run("VR template references");
    ok = testVRTemplateReferences(repo_root) && ok;
    run("LevelManager H-B JSON/binary identity and hierarchy restore");
    ok = testLevelManagerHBPersistenceAndCohortRestore() && ok;
    run("LevelManager H-B rejects malformed cohorts before mutation");
    ok = testLevelManagerHBRejectsBeforeRegistryMutation() && ok;
    run("LevelManager rolls back failed scoped cohort restore");
    ok = testLevelManagerReflectedFailureRollback() && ok;
    ok = testLevelManagerScopedRestoreRollback() && ok;
    ok = testLevelManagerPostCreationRollback() && ok;
    ok = testLevelManagerLargeIdentityCohort() && ok;

    Assets::AssetManager::get().shutdown();
    EE::CLog::Shutdown();
    return ok ? 0 : 1;
}
