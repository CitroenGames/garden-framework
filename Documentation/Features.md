# Features

### Rendering
*   **Multi-API Renderer**: Vulkan, Direct3D 12, Metal, and a headless backend for dedicated servers.
*   **Deferred Rendering Pipeline**: Two-phase GBuffer architecture. Phase 1 fills three MRTs per frame: RT0 (BaseColor RGB + Metallic A, RGBA8), RT1 (World-space Normal RGB + Roughness A, RGBA16F), RT2 (Emissive RGB + AO A, RGBA16F) plus a depth buffer. Phase 2 runs a deferred lighting pass reading all four buffers to produce an HDR result, followed by skybox, transparent-forward, and the full post-process chain.
*   **Render Graph**: Backend-agnostic, compile-time-validated render graph (`RGTextureHandle` typed handles, resource lifetime tracking) with Vulkan and D3D12 backends. Orchestrates all passes from GBuffer fill through post-processing and ImGui composite into a single DAG-scheduled frame.
*   **Slang Shader System**: Unified shader source compiled to SPIR-V (Vulkan), DXIL (D3D12), and MSL (Metal) via Slang 2026.5.2.
*   **Cascaded Shadow Maps (CSM)**: 4-cascade directional shadows at 4096x4096 with PCF filtering and cascade blending.
*   **Frustum Culling**: BVH-accelerated spatial culling for efficient rendering.
*   **Post-Processing**: Composable post-process chain executed via the render graph — SSAO (raw + separable bilateral blur), directional shadow mask, HDR tonemapping, and FXAA anti-aliasing. Each stage is optional and toggled per `PostProcessGraphBuilder::Config`.
*   **Skybox**: Procedural atmosphere rendering.
*   **Multi-Material Meshes**: Per-submesh textures and material ranges from glTF.
*   **Debug Drawing**: Wireframe lines, boxes, spheres, capsules, and rays for visualizing physics and spatial data.

### Editor
*   **Dockable Panel Layout**: ImGui-based editor with viewport, scene hierarchy, inspector, content browser, console, and 14+ panels.
*   **Scene Viewport**: Renders the scene to an offscreen texture with an orbiting editor camera (WASD + right-click) and viewport overlay controls.
*   **Gizmos**: Translate, rotate, and scale via ImGuizmo with snap support and a view orientation cube.
*   **Scene Hierarchy & Inspector**: Select entities, edit transforms, mesh, physics, audio, animation, and IK components per-entity. Collider type selection widget.
*   **Content Browser**: Browse project assets with metadata display and context-menu reimport.
*   **Model Preview Panel**: Inline 3D preview of mesh assets.
*   **LOD Settings Panel**: Configure mesh LOD levels and thresholds with meshoptimizer integration.
*   **NavMesh Panel**: Generate, visualize, save/load navigation meshes and test pathfinding.
*   **Physics Debug Panel**: Visualize colliders, AABBs, and contact points at runtime.
*   **Level Settings Panel**: Edit level-wide properties (lighting, environment).
*   **Play In Editor (PIE)**: Enter play mode with world snapshot/restore, pause, eject to free-cam, and re-enter. Network PIE support with multi-process client instances.
*   **Prefab Editor**: Create, edit, and spawn prefabs with nested prefab support and hot-reload.
*   **Undo/Redo**: Action-based undo system for editor operations.
*   **Project Browser**: Create new projects from templates (EmptyProject, FPSShooter) or open existing `.garden` project files.
*   **Level Serialization**: New, open, save, and save-as for JSON level files with native file dialogs.
*   **Console Panel**: Integrated developer console with command input, tab completion, and log filtering.
*   **Toolbar & Status Bar**: Quick-access editor actions and status information.

### UI
*   **RmlUi Integration**: HTML/CSS-based game UI for HUDs, menus, and other in-game interfaces. Per-backend renderers for Vulkan, D3D12, and Metal.

### Engine Systems
*   **Entity Component System (ECS)**: `entt`-based with transform, mesh, rigidbody, collider, audio source, animation, IK, input, camera, and prefab instance components.
*   **Reflection System**: Macro-free C++ reflection with property registration, Unreal-style specifiers (EditAnywhere, VisibleAnywhere), automatic editor widget generation, and JSON serialization. Game modules register custom components at runtime.
*   **Physics**: Jolt Physics 5.5.0 with rigid bodies, capsule character controllers, raycasting, collision layers, and fixed-timestep simulation.
*   **Audio**: miniaudio-based spatial audio system with 3D positional sound, audio groups (SFX, Music, Voice, UI), and per-group volume control.
*   **Animation**: Skeletal animation with bone hierarchies, keyframe interpolation (SLERP), animation blending/crossfade, bone masks, animation layers, and glTF skin/animation loading. Skinned vertex shaders for all backends.
*   **Inverse Kinematics**: Two-Bone analytical IK (law of cosines with pole vector hints) and FABRIK iterative solver for arbitrary-length chains, both with weight blending.
*   **Event Bus**: Decoupled communication between systems via `entt::dispatcher` with immediate and deferred event dispatch.
*   **Timer System**: Gameplay timers with cooldowns, delays, pause/resume, and global time scaling.
*   **Game State Manager**: Stack-based state machine for game flow (playing, paused, menus) with transparent overlay support.
*   **Scene Manager**: Level lifecycle management with load/unload/transition, wrapping the JSON-based level system.
*   **Networking**: ENet-based client-server multiplayer with world state replication, delta compression, input command streaming, and client-side interpolation.
*   **Asset Pipeline**: Async asset loading with thread pool, GPU upload scheduling, and loader plugin architecture. Supports glTF/GLB and OBJ.
*   **Asset Compiler**: Multithreaded offline compilation of models (`.cmesh`) and textures (`.ctex`) with BC1/BC3/BC5/BC7 compression, automatic mipmap generation, LOD generation, and incremental builds.
*   **Prefab System**: Save, load, and spawn entity prefabs from JSON files with position overrides and hot-reload.
*   **Console System**: Source Engine-style ConVars with typed values, flags (ARCHIVE, REPLICATED, CHEAT), bounds validation, config save/load, and network replication.
*   **Job System**: Multi-threaded work scheduling with priorities, dependencies, barriers, and a main-thread queue for GPU operations.
*   **Input System**: SDL3-based with per-frame key state tracking, mouse delta, action mapping, and delegate callbacks.
*   **Data-Driven Levels**: JSON and binary level formats with per-entity transform, mesh, physics, and component configuration.
*   **Model Support**: Loads `.gltf`/`.glb` (with materials and skeletal data) and `.obj` models.

### Game Modules
*   **Runtime Module Loading**: Game logic ships as a DLL loaded at runtime (API version 3). Hot-reload via copy-on-load without file locks.
*   **Module Interface**: Init, shutdown, update, level-loaded, play-start/stop hooks. Optional server-side hooks for dedicated servers (client connect/disconnect, server update).
*   **EngineServices**: Game modules receive the world, renderer, input, reflection registry, level manager, and network PIE settings.
*   **Custom Components**: Modules register reflected components via `gardenRegisterComponents`, making them editable in the inspector.

### Templates
*   **EmptyProject**: Minimal starter with directory structure, game module entry point, and a default level.
*   **FPSShooter**: Complete networked FPS template with client-server architecture, weapon system, character animation, HUD, movement prediction, and delta-compressed replication.

### Platforms
*   **Windows**: Direct3D 12 and Vulkan.
*   **Linux**: Vulkan.
*   **macOS**: Metal and Vulkan.
