# Physics

Garden uses [Jolt Physics 5.5.0](https://github.com/jrouwe/JoltPhysics). The engine owns one `PhysicsSystem`, which owns the Jolt world, and steps it at a fixed timestep from `world::step_physics`.

You access physics through `g_services->game_world->getPhysicsSystem()` for advanced calls, or through the convenience wrappers on `world` itself.

## Adding a body

A "physics body" in the ECS is an entity with both a collider component and (optionally) a rigid-body component:

```cpp
auto& reg = g_services->game_world->registry;
auto e = reg.create();

reg.emplace<TransformComponent>(e, 0.0f, 5.0f, 0.0f);
reg.emplace<MeshComponent>(e).m_mesh = loadMesh("assets/models/crate.glb");

// Box collider sized to mesh AABB
ColliderBoxComponent box;
box.half_extents = {0.5f, 0.5f, 0.5f};
reg.emplace<ColliderBoxComponent>(e, box);

// Dynamic rigid body
RigidBodyComponent rb;
rb.mass          = 10.0f;
rb.apply_gravity = true;
reg.emplace<RigidBodyComponent>(e, rb);
```

When the entity is created, the engine builds a Jolt body from the components on the next physics step. Editing `TransformComponent.position` from code teleports the body; `RigidBodyComponent.velocity` sets linear velocity.

For exact field names per collider shape, see the headers in `Engine/src/Physics/`.

## Collider shapes

| Component | Shape |
| :--- | :--- |
| `ColliderBoxComponent` | Box (half-extents) |
| `ColliderSphereComponent` | Sphere (radius) |
| `ColliderCapsuleComponent` | Capsule (radius, half-height) |
| `ColliderMeshComponent` | Triangle mesh from a model — static use only |

Pick the simplest shape that fits. Mesh colliders are expensive — use them for static level geometry, not dynamic bodies.

## Static vs dynamic

`RigidBodyComponent.mass == 0` (or no rigid body at all, just a collider) → static. Static bodies don't move and don't cost CPU per step. Add a rigid body only when the entity should respond to forces.

## Character controller

For player movement use `CharacterControllerComponent` rather than a raw rigid body. It's a Jolt capsule character controller — handles ground detection, slope limits, step-up, water level — driven by `CharacterMoveInput`:

```cpp
CharacterMoveInput input;
input.move_forward = forward_amount;   // -1..1
input.move_right   = right_amount;
input.camera_yaw   = camera.yaw;
input.camera_pitch = camera.pitch;
if (jumpPressed) input.buttons |= CharacterMoveFlags::Jump;

// Each frame, feed input into the controller. The PhysicsSystem applies it during step.
```

The FPSShooter template's `PlayerController.hpp` and `Network/SharedMovement.hpp` are the canonical example, including how to feed identical input into client prediction and server authority for multiplayer.

## Raycasts and shape casts

Convenience wrappers on `world`:

```cpp
auto& w = *g_services->game_world;

// Simple yes/no raycast
glm::vec3 hitPoint, hitNormal;
if (w.raycast(origin, direction, max_dist, hitPoint, hitNormal)) {
    /* hit */
}

// Closest hit, optionally excluding one entity (e.g., the shooter)
auto result = w.raycastClosest(origin, direction, max_dist, /*ignore=*/shooter);
if (result.hit) {
    spawnImpact(result.hit_point, result.hit_normal);
    if (result.entity != entt::null) doDamage(result.entity);
}

// Sphere cast (origin, radius, direction, max distance)
auto sweep = w.sphereCast(origin, 0.3f, direction, 10.0f);
```

`PhysicsSystem` exposes more advanced queries (overlap, capsule cast, collision filters). See `Engine/src/PhysicsSystem.hpp`.

## Fixed timestep

`world::step_physics(dt)` uses the shared `Tick::FixedTickAccumulator` to accumulate real time and run as many fixed steps as needed (capped at 8 to avoid spiral of death). It returns the number of fixed steps consumed during the call. Default `fixed_delta` is set by `PhysicsSystemSettings`, and `world::getPhysicsInterpolationAlpha()` exposes the remaining partial-step alpha for interpolation.

The engine drives this for you — you don't call `step_physics` from gameplay. But it means:

- Physics state changes between frames are interpolated; gameplay reads "current" state which may have stepped 0, 1, or many times since last frame.
- Apply impulses *before* the step happens by editing `RigidBodyComponent` fields (or pushing to its impulse queue, see component header).
- If you need deterministic networking, drive physics yourself from a fixed gameplay tick — the FPSShooter does this server-side.

## Debug visualisation

The Physics Debug Panel in the editor toggles wireframes for colliders, AABBs, and contact points. From C++:

```cpp
#include "Debug/DebugDraw.hpp"
DebugDraw::get().drawLine(a, b, glm::vec3(1, 0, 0), 0.5f /* lifetime */);
DebugDraw::get().drawSphere(center, 0.25f, glm::vec3(0, 1, 0));
```

Debug draw is disabled in release builds when no panel is open, so it's cheap to leave in code.

## Layers and filters

Jolt collision layers are configured engine-side in `PhysicsSystem`. Default layers cover Static, Dynamic, and Character. For custom filtering (e.g., bullets ignoring teammates) you currently filter in the raycast result instead of through Jolt — pass the shooter entity to `raycastClosest` and check ownership on the hit entity.
