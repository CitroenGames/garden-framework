# Garden Framework — Agent Guide

## What this project is

Garden Framework is a cross-platform C++20 game engine built for FPS-style games, with a full editor and runtime. It is split into two DLLs and three executables:

```text
EngineCore.dll          EngineGraphics.dll
 ECS, Physics, Audio     Vulkan, D3D12, Metal
 Assets, Animation       ImGui, RmlUi, ImGuizmo
 Navigation, Console     Render API abstraction
 Job System, Levels
 Input, Networking
       |                        |
       +------+-------+---------+
              |       |         |
           Game.exe  Editor.exe  Server.exe
           (Client)  (Editor)   (Headless)
```

- **EngineCore** (`Engine/src/`, exports via `ENGINE_API`): EnTT ECS, Jolt physics, miniaudio, skeletal animation, asset pipeline, Recast navigation, console/ConVars, job system, level serialization, ENet networking, input. Links SDL3, GLM, spdlog, EnTT, ENet, Jolt, miniaudio, meshoptimizer, tinygltf.
- **EngineGraphics** (`Engine/src/Graphics/`): multi-backend render API (Vulkan, D3D12, Metal, headless), ImGui, RmlUi, ImGuizmo.
- **Game logic** is loaded at runtime as a game module DLL via `GameModuleLoader`. Templates live in `Templates/` (EmptyProject, FPSShooter, ThirdPerson, VR).
- The current product goal: evolve this into a commercial-ready general-purpose engine, validated by building the **FPSShooter template** into a complete playable multiplayer FPS (round-based teams, buy economy, bomb defusal, bots, HUD). `I:\github\OpenStrike` is the gameplay-requirements reference only — never copy its code or assets. Always name the game only "FPSShooter template".
- `plan/` is the living engineering blueprint for that goal: `plan/GOAL.md`, subsystem docs under `plan/engine/` and `plan/game/`, architecture rulings in `plan/decisions/`, and `plan/IMPLEMENTATION_ORDER.md`. Read the owning plan doc before implementing a subsystem.

## Repository layout

| Path | Contents |
|---|---|
| `Engine/src/` | EngineCore + EngineGraphics sources (Components, ECS, Events, Physics, Network, Animation, Assets, UI, Project, Reflection, ...) |
| `Engine/Thirdparty/` | Vendored deps: entt, JoltPhysics-5.5.0, SDL3-3.4.4, enet, glm, spdlog, miniaudio, recastnavigation, RmlUi, imgui, meshoptimizer, tinygltf |
| `Game/Client/` | `Game.exe` standalone client |
| `Game/Server/` | `Server.exe` headless dedicated server (no graphics) |
| `Game/Editor/` | Editor with Play-In-Editor |
| `Templates/FPSShooter/` | FPS template: game module (`src/`), assets, levels, `FPSShooter.garden` project file |
| `Tests/` | Per-suite test executables (GameplayTests, PhysicsTests, NetworkStressTests, PackagerTests, EntityLifecycleTests, HierarchyTests, ...), each with its own `.buildscript` |
| `Tools/` | GardenCLI, NetworkTests, CrashDbg, slang toolchain |
| `Docs/`, `Documentation/` | User/developer docs (not committed by agents, see commit rules) |
| `plan/` | Engineering blueprint for the FPSShooter goal (not committed, see commit rules) |
| `build/` | Generated VS projects/solutions (sighmake output, not source of truth) |
| `bin/` | Built executables and DLLs |

## How to compile

Build system is **sighmake** (buildscripts, not CMake). Generate projects, then build with MSBuild.

```bat
:: Generate Visual Studio projects (writes build\Garden_.slnx)
sighmake project.buildscript
:: or use the provided wrapper, which also copies SDL3.dll into bin\
Generate SLN.bat
```

- If `sighmake` is not on PATH, a working binary is at `I:\github\PhysStrike\sighmake.exe`.
- Open `build/Garden_.slnx` in Visual Studio and build `Debug|x64` or `Release|x64`, or build a single project from the command line (one project per invocation):

```bat
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" build\<Project>_.vcxproj /p:Configuration=Release /p:Platform=x64
```

- **Agents: always build Release.** Debug currently fails to link EngineCore (pre-existing MDd/MD runtime-library mismatch in third-party libs).
- Compile shaders when rendering changes: `compile_shaders_slang.bat`.
- Adding a target: create its `.buildscript` (see any `Tests/*/*.buildscript`) and register it (tests: an `include` line in `Tests/Tests.buildscript`), then re-run sighmake. If generation drops a new project, clone an existing test `.vcxproj` in `build/` for local verification only — the buildscript remains the source of truth.
- Runtime dependency on Windows is `SDL3.dll` (from `Engine\Thirdparty\SDL3-3.4.4\lib\x64\`), copied next to the executables in `bin\`.

## How to run

```bat
bin\Editor.exe -d3d12
bin\Game.exe
bin\Server.exe --project Templates\FPSShooter\FPSShooter.garden
```

`Server.exe` requires a `.garden` project (`--project <path>` or a `.garden` file beside it) and exits with a fatal log otherwise. That is expected behaviour, not a crash.

## How to test

- Each suite in `Tests/` builds to `bin\<Suite>.exe` and prints `[PASS]`/`[FAIL]` lines; exit code 0 means pass.
- Run the suite covering your change plus adjacent suites. Currently relevant: `EntityLifecycleTests`, `HierarchyTests`, `GameplayTests`, `PackagerTests`, `ReflectionTests`, `PhysicsTests`, `NetworkStressTests`.
- Launch-test runtime changes: `bin\Server.exe --project Templates\FPSShooter\FPSShooter.garden` must start and stay running headless.
- Multiplayer claims need real processes (1x `Server.exe` + 2x `Game.exe`), not loopback-only reasoning.
- Never claim a pass you did not observe in output.

## Working rules for agents

- One workflow at a time, one focused task per workflow. Roles: implementer (Wonder), verifier (muse-spark-1.3), fixer (only when verification fails), then re-verify.
- Read the owning `plan/` doc and the actual code before designing or editing. If plan and code disagree, update the plan doc first — do not silently diverge, and do not invent APIs the code does not have.
- Keep EngineCore generic (no game-specific types); game rules live in the game module/template. Prefer ECS components + systems.
- Remove deprecated code instead of adding compatibility shims.
- A feature is complete only when it builds, its tests pass, and it works in a real run — not when its API exists.

## Commit rules

- Commit code and tests, staged by named file. Never `git add .` in a dirty tree.
- No Markdown files in commits, except this `agents.md`. `plan/` and all other `.md` files stay uncommitted in the working tree.
- No push, amend, rebase, cherry-pick, or tag unless the user explicitly asks in this session.
