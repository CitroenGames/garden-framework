# Garden Framework — Game Developer Docs

These docs target developers **building a game** in Garden Framework. For engine internals, build instructions, and feature lists, see `../Documentation/`.

## Read these first

1. [Getting Started](getting-started.md) — install the engine, create a project, build, run.
2. [Project Structure](project-structure.md) — what lives in a `.garden` project.
3. [Game Module](game-module.md) — the DLL your game logic ships in.
4. [ECS & Components](ecs-and-components.md) — entities, components, and reflection.

## Reference by topic

| Topic | What it covers |
| :--- | :--- |
| [Levels & Prefabs](levels-and-prefabs.md) | Level JSON, scene manager, spawning prefabs |
| [Input](input.md) | Keys, mouse, action bindings |
| [Physics](physics.md) | Rigid bodies, colliders, raycasts, character controller |
| [Rendering](rendering.md) | Meshes, materials, cameras, shaders |
| [Audio](audio.md) | Spatial audio, audio groups |
| [Networking](networking.md) | Client/server, replication, prediction |
| [UI](ui.md) | RmlUi HUDs and menus |
| [Console & ConVars](console-and-convars.md) | Cvars, dev console |
| [Dedicated Server](dedicated-server.md) | Headless server module |
| [Editor Workflow](editor-workflow.md) | Day-to-day editor use |

## Conventions used in this doc

- File paths are relative to the project root unless prefixed with `Engine/` (then they are inside the engine repo).
- Code examples assume you are inside `gardenGameInit`/`gardenGameUpdate` and have an `EngineServices* g_services` cached.
- "Host" means whichever executable loads your game DLL: `Editor.exe` (PIE), `Game.exe` (client), or `Server.exe` (dedicated).

## Starter templates

| Template | When to start here |
| :--- | :--- |
| `Templates/EmptyProject` | You want a clean canvas. Single-player, minimal hooks, no networking. |
| `Templates/ThirdPerson` | You want a small single-player character controller with a spring-arm third-person camera. |
| `Templates/VR` | You want an OpenXR-ready walkaround pawn with keyboard/mouse fallback for development. |
| `Templates/FPSShooter` | You want a working multiplayer FPS to copy from: client/server, weapons, HUD, prediction. |

These templates can be generated through `garden new-project` (see [Getting Started](getting-started.md)) or copied by hand.
