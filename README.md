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

### Editor
*   **Dockable Panel Layout**: ImGui-based editor with viewport, scene hierarchy, inspector, content browser, console, and more.
*   **Scene Viewport**: Renders the scene to an offscreen texture with an orbiting editor camera (WASD + right-click).
*   **Gizmos**: Translate, rotate, and scale via ImGuizmo with snap support and a view orientation cube.
*   **Scene Hierarchy & Inspector**: Select entities, edit transforms, mesh, physics, and audio components per-entity.
*   **Content Browser**: Browse project assets with metadata display and context-menu reimport.
*   **LOD Settings Panel**: Configure mesh LOD levels and thresholds with meshoptimizer integration.
*   **NavMesh Panel**: Generate, visualize, save/load navigation meshes and test pathfinding.
*   **Physics Debug Panel**: Visualize colliders, AABBs, and contact points at runtime.
*   **Play In Editor (PIE)**: Enter play mode with world snapshot/restore, pause, eject to free-cam, and re-enter.
*   **Undo/Redo**: Action-based undo system for editor operations.
*   **Project Browser**: Create new projects from templates or open existing `.garden` project files.
*   **Level Serialization**: New, open, save, and save-as for JSON level files with native file dialogs.
*   **Console Panel**: Integrated developer console with command input, tab completion, and log filtering.

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

## Architecture

The engine is split into two DLLs and three executables:

```
EngineCore.dll          EngineGraphics.dll
 ECS, Physics, Audio     Vulkan, D3D11, Metal
 Assets, Animation       ImGui, RmlUi, ImGuizmo
 Navigation, Console     Render API abstraction
 Job System, Levels      stb_image, Debug Draw UI
 Input, Networking
       |                        |
       +------+-------+---------+
              |       |         |
           Game.exe  Editor.exe  Server.exe
           (Client)  (Editor)   (Headless)
```

**EngineCore** (`EngineCore.dll`) contains all platform-independent game logic: the ECS (entt), Jolt physics, miniaudio, skeletal animation, asset pipeline, navigation, console/ConVar system, job system, level serialization, networking, and input management. It exports symbols via `ENGINE_API` and links SDL2, GLM, spdlog, entt, ENet, Jolt Physics, miniaudio, meshoptimizer, tinygltf, and tinyobjloader.

**EngineGraphics** (`EngineGraphics.dll`) contains all rendering and UI code: the multi-backend render API (Vulkan, D3D11, Metal, headless factory), ImGui integration, RmlUi integration, and ImGuizmo gizmos. It exports symbols via `ENGINE_GRAPHICS_API` and links EngineCore, vk-bootstrap, VMA, Dear ImGui, ImGuizmo, and RmlUi.

**Game** is the standalone client with a game loop, player controller, and game module loader. **Editor** is the Unreal-style level editor built on ImGui with dockable panels, Play-In-Editor, and gizmo manipulation. **Server** is a headless dedicated server that uses EngineCore without any graphics dependencies for authoritative game simulation.

Game logic can be loaded at runtime via `GameModuleLoader`, which dynamically loads a shared library implementing the game's custom components, systems, and update loop.

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
| **Game/Editor/** | Unreal-style level editor with dockable panels |
| **Game/Server/** | Dedicated headless server |
| **Game/shared/** | Network protocol and shared components |
| **assets/shaders/** | GLSL, HLSL, Metal, and SPIR-V shaders |
| **assets/levels/** | JSON level definitions |
| **assets/models/** | 3D models (glTF, OBJ) |
