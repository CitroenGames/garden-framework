# Garden Framework

## Description

A cross-platform 3D game engine written in C++20, built as a foundation for FPS-style games. Supports Vulkan, Direct3D 11, and Metal rendering backends. Runs on Windows, Linux, and macOS.

![screenshot](IMG/screen.png)
![screenshot](IMG/editor.png)


## Features

### Rendering
*   **Multi-API Renderer**: Vulkan, Direct3D 11, Metal, and a headless backend for dedicated servers.
*   **Cascaded Shadow Maps (CSM)**: 4-cascade directional shadows with PCF filtering and cascade blending.
*   **Frustum Culling**: BVH-accelerated spatial culling for efficient rendering.
*   **Post-Processing**: FXAA anti-aliasing via offscreen framebuffer pipeline.
*   **Skybox**: Procedural atmosphere rendering.
*   **Multi-Material Meshes**: Per-submesh textures and material ranges from glTF.
*   **Debug Drawing**: Wireframe lines, boxes, spheres, capsules, and rays for visualizing physics and spatial data.

### Engine Systems
*   **Entity Component System (ECS)**: `entt`-based with transform, mesh, rigidbody, collider, player, audio source, and animation components.
*   **Physics**: Jolt Physics 5.5.0 with rigid bodies, capsule character controllers, raycasting, collision layers, and fixed-timestep simulation.
*   **Audio**: miniaudio-based spatial audio system with 3D positional sound, audio groups (SFX, Music, Voice, UI), and per-group volume control.
*   **Animation**: Skeletal animation with bone hierarchies, keyframe interpolation (SLERP), animation blending/crossfade, and glTF skin/animation loading. Skinned vertex shaders for all backends.
*   **Event Bus**: Decoupled communication between systems via `entt::dispatcher` with immediate and deferred event dispatch.
*   **Timer System**: Gameplay timers with cooldowns, delays, pause/resume, and global time scaling.
*   **Game State Manager**: Stack-based state machine for game flow (playing, paused, menus) with transparent overlay support.
*   **Scene Manager**: Level lifecycle management with load/unload/transition, wrapping the JSON-based level system.
*   **Networking**: ENet-based client-server multiplayer with world state replication, delta compression, and input command streaming.
*   **Asset Pipeline**: Async asset loading with thread pool, GPU upload scheduling, and loader plugin architecture. Supports glTF/GLB and OBJ.
*   **Console System**: Source Engine-style ConVars with typed values, flags (ARCHIVE, REPLICATED, CHEAT), bounds validation, config save/load, and network replication.
*   **Job System**: Multi-threaded work scheduling with priorities, dependencies, barriers, and a main-thread queue for GPU operations.
*   **Input System**: SDL2-based with per-frame key state tracking, mouse delta, action mapping, and delegate callbacks.
*   **Data-Driven Levels**: JSON and binary level formats with per-entity transform, mesh, physics, and component configuration.
*   **Model Support**: Loads `.gltf`/`.glb` (with materials and skeletal data) and `.obj` models.

## Building

See [build.md](Documentation/build.md) for prerequisites, build instructions, and running the engine.

## Level Format (.json)

Levels are defined in JSON format. Example configuration for lighting:

```json
"lighting": {
  "ambient": { "r": 0.2, "g": 0.2, "b": 0.2 },
  "diffuse": { "r": 0.8, "g": 0.8, "b": 0.8 },
  "direction": { "x": 0.0, "y": -1.0, "z": -0.5 }
}
```

## Project Structure

| Directory | Description |
| :--- | :--- |
| **Engine/src/Graphics/** | Multi-API rendering, shaders, post-processing, BVH, debug draw |
| **Engine/src/Components/** | ECS components (transform, mesh, physics, audio, animation) |
| **Engine/src/Audio/** | miniaudio-based spatial audio system |
| **Engine/src/Animation/** | Skeletal animation, blending, glTF loader |
| **Engine/src/Events/** | Event bus and predefined engine events |
| **Engine/src/Timer/** | Gameplay timer/scheduler system |
| **Engine/src/GameState/** | Stack-based game state manager |
| **Engine/src/Scene/** | Scene lifecycle and level transitions |
| **Engine/src/Console/** | ConVar system and developer console |
| **Engine/src/Threading/** | Job system and thread pool |
| **Engine/src/Assets/** | Async asset manager and loaders |
| **Engine/src/Debug/** | Debug drawing system |
| **Engine/Thirdparty/** | Third-party dependencies |
| **Game/Client/** | Client executable and player controller |
| **Game/Server/** | Dedicated headless server |
| **Game/shared/** | Network protocol and shared components |
| **assets/shaders/** | GLSL, HLSL, Metal, and SPIR-V shaders |
| **assets/levels/** | JSON level definitions |
| **assets/models/** | 3D models (glTF, OBJ) |
