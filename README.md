<div align="center">
  <img src="assets/Editor/Icons/GardenLogo.png" alt="Garden Framework logo" width="180">

  <h1>Garden Framework</h1>

  <p><strong>A cross-platform C++20 game engine built for FPS-style games, fast iteration, and real editor workflows.</strong></p>

  <p>
    <a href="Documentation/Features.md">Features</a>
    ·
    <a href="Documentation/build.md">Build Guide</a>
    ·
    <a href="Docs/getting-started.md">Getting Started</a>
    ·
    <a href="Templates">Templates</a>
  </p>

  <p>
    <img alt="C++20" src="https://img.shields.io/badge/C%2B%2B-20-00599C?style=for-the-badge">
    <img alt="Render backends" src="https://img.shields.io/badge/Rendering-Vulkan%20%7C%20D3D12%20%7C%20Metal-3A7D44?style=for-the-badge">
    <img alt="Platforms" src="https://img.shields.io/badge/Platforms-Windows%20%7C%20Linux%20%7C%20macOS-555555?style=for-the-badge">
  </p>
</div>

## Overview

Garden Framework is a game engine foundation with a multi-backend renderer, a full scene editor, runtime game module loading, dedicated server support, and a full asset pipeline. It is focused on shipping practical game systems rather than being a renderer demo: ECS, physics, audio, animation, networking, navigation, editor tooling, templates, and build scripts live in one repo.

| Editor | Runtime |
| :---: | :---: |
| ![Garden editor](IMG/editor.png) | ![Garden runtime](IMG/screen.png) |

## What You Get

| Area | Highlights |
| :--- | :--- |
| Rendering | Vulkan, Direct3D 12, Metal, render API abstraction, shaders, ImGui, RmlUi, ImGuizmo |
| Editor | Dockable panels, Play-In-Editor, project browser, gizmos, level editing, reflected components |
| Gameplay | ECS, Jolt physics, input, audio, skeletal animation, navigation, jobs, console variables |
| Assets | Meshes, textures, levels, prefabs, imported assets, and runtime-ready project packaging |
| Networking | Client, dedicated server, ENet transport, shared gameplay protocol support |
| Templates | Empty project, FPS shooter, third person, and VR starter projects |

See [Features.md](Documentation/Features.md) for the complete feature list.

## Quick Start

```bat
sighmake project.buildscript
sighmake --build . --config Debug --platform x64 --parallel 8
compile_shaders_slang.bat
bin\Editor.exe -d3d12
```

For Visual Studio on Windows:

```bat
Generate SLN.bat
```

Then open `build/Garden_.slnx` and build `Debug|x64` or `Release|x64`.

Linux and macOS builds are documented in [build.md](Documentation/build.md).

## Templates

| Template | Purpose |
| :--- | :--- |
| [EmptyProject](Templates/EmptyProject) | Minimal game module starting point |
| [FPSShooter](Templates/FPSShooter) | FPS controller, weapons, networking, HUD, terrain and scene examples |
| [ThirdPerson](Templates/ThirdPerson) | Third-person camera and character controller starter |
| [VR](Templates/VR) | OpenXR-oriented walkaround starter with desktop fallback controls |

New projects can be created from the editor project browser or from the command line. See [getting-started.md](Docs/getting-started.md) for the full workflow.

## Architecture

The engine is split into two DLLs and three executables:

```text
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

**Game** is the standalone client with a game loop, player controller, and game module loader. **Editor** is the dockable level editor built on ImGui with Play-In-Editor and gizmo manipulation. **Server** is a headless dedicated server that uses EngineCore without graphics dependencies for authoritative game simulation.

Game logic can be loaded at runtime via `GameModuleLoader`, which dynamically loads a shared library implementing the game's custom components, systems, and update loop.

## Building

See [build.md](Documentation/build.md) for prerequisites, build instructions, shader compilation, platform notes, and runnable targets.
