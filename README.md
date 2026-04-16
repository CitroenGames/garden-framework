# Garden Framework

## Description

A cross-platform 3D game engine written in C++20, built as a foundation for FPS-style games. Supports Vulkan, Direct3D 12, and Metal rendering backends. Runs on Windows, Linux, and macOS.

![screenshot](IMG/screen.png)
![screenshot](IMG/editor.png)


## Features

Multi-backend rendering (Vulkan, D3D12, Metal), an Unreal-style level editor with dockable panels and Play-In-Editor, ECS with physics, audio, skeletal animation, networking, and a full asset pipeline.

See [Features.md](Documentation/Features.md) for the complete feature list.

## Architecture

The engine is split into two DLLs and three executables:

```
EngineCore.dll          EngineGraphics.dll
 ECS, Physics, Audio     Vulkan, D3D12, Metal
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

**EngineCore** (`EngineCore.dll`) contains all platform-independent game logic: the ECS (entt), Jolt physics, miniaudio, skeletal animation, asset pipeline, navigation, console/ConVar system, job system, level serialization, networking, and input management. It exports symbols via `ENGINE_API` and links SDL3, GLM, spdlog, entt, ENet, Jolt Physics, miniaudio, meshoptimizer, tinygltf, and tinyobjloader.

**EngineGraphics** (`EngineGraphics.dll`) contains all rendering and UI code: the multi-backend render API (Vulkan, D3D12, Metal, headless factory), ImGui integration, RmlUi integration, and ImGuizmo gizmos. It exports symbols via `ENGINE_GRAPHICS_API` and links EngineCore, vk-bootstrap, VMA, Dear ImGui, ImGuizmo, and RmlUi.

**Game** is the standalone client with a game loop, player controller, and game module loader. **Editor** is the Unreal-style level editor built on ImGui with dockable panels, Play-In-Editor, and gizmo manipulation. **Server** is a headless dedicated server that uses EngineCore without any graphics dependencies for authoritative game simulation.

Game logic can be loaded at runtime via `GameModuleLoader`, which dynamically loads a shared library implementing the game's custom components, systems, and update loop.

## Building

See [build.md](Documentation/build.md) for prerequisites, build instructions, and running the engine.
