# Project Structure

A Garden project is just a folder with a `.garden` descriptor, a `.buildscript`, source code, and assets. Nothing is hidden inside the engine repo.

## The `.garden` file

A small JSON file the engine and CLI read to know what your project is.

```json
{
    "name": "MyGame",
    "engine_id": "",
    "engine_version": "1.0",
    "game_module": "bin/MyGame",
    "default_level": "assets/levels/main.level.json",
    "default_game_mode": "GameMode",
    "default_game_state": "GameState",
    "asset_directories": ["assets/"],
    "source_directory": "src/",
    "buildscript": "MyGame.buildscript"
}
```

| Field | Meaning |
| :--- | :--- |
| `name` | Display name for the project browser. |
| `engine_id` | Set by `garden set-engine` — pins this project to a registered engine. Empty means "any engine". |
| `game_module` | Path (no extension) to the game DLL the host should load. The engine appends `.dll` / `.so` / `.dylib`. |
| `default_level` | Level the editor opens when the project loads, and the level Game.exe starts in. |
| `default_game_mode` | Project default GameMode class. Levels can override this in Level Settings. |
| `default_game_state` | Project default GameState class mirrored into the ECS. |
| `asset_directories` | Roots scanned by the asset compiler and content browser. |
| `source_directory` | Hint to the editor for "open source folder" actions. |
| `buildscript` | Sighmake buildscript that produces the game DLL. |

## The `.buildscript` file

Sighmake input. Defines the DLL target and pulls in the engine SDK.

```ini
[solution]
name = MyGame
platforms = x64, Linux

include = ${ENGINE_PATH}/Engine/Engine.buildscript

[project:MyGame]
type      = dll
sources   = src/**/*.cpp
headers   = src/**/*.hpp
includes  = src
target_link_libraries(
    PRIVATE EngineSDK
)
outdir       = bin
multiprocessor = true
simd         = AdvancedVectorExtensions2
std          = 20
utf8         = true
```

Things you will edit:

- **`sources` / `headers`** — globs that pick up your `src/` tree. Defaults usually work.
- **`includes`** — extra include roots. Add subfolders you want unqualified `#include` access to.
- **`target_link_libraries`** — third-party libs you want linked into the DLL.
- **`type = dll`** — never change this. The host loads a DLL.

The `${ENGINE_PATH}` variable is set on the `sighmake` command line (`-D ENGINE_PATH=...`) or by GardenCLI when generating.

> **Reminder:** When you add a new source file, add it to the `.buildscript` (or trust the glob) and re-run `sighmake`. The engine's CI also rebuilds against `.buildscript` files, so updating only the IDE's `.vcxproj` is not enough.

## Recommended directory layout

```
MyGame/
├── MyGame.garden
├── MyGame.buildscript
├── src/
│   ├── GameModule.cpp           # DLL entry — required exports live here
│   ├── Components/              # Custom ECS components
│   ├── Systems/                 # Per-frame logic
│   └── shared/                  # Code shared with server module (multiplayer)
├── assets/
│   ├── levels/                  # *.level.json
│   ├── models/                  # *.glb, *.gltf, *.obj (source) + .cmesh (compiled)
│   ├── textures/                # *.png, *.jpg (source) + .ctex (compiled)
│   ├── shaders/                 # custom shaders, if any
│   ├── prefabs/                 # *.prefab
│   └── ui/                      # *.rml, *.rcss for RmlUi HUDs
└── bin/
    └── MyGame.dll               # build output
```

The editor will scan `asset_directories`, so anything under `assets/` shows up in the Content Browser.

## Compiled vs. source assets

The asset compiler (`GardenCLI` or the editor's reimport menu) turns:

- `*.gltf` / `*.glb` / `*.obj` → `*.cmesh` (LODs, optimised vertex layout)
- `*.png` / `*.jpg` → `*.ctex` (BC1/3/5/7 compressed, mipmaps)

At runtime the engine prefers compiled forms. Source files are kept around so you can edit them and reimport. Don't ship raw PNG/GLB to players — ship `.ctex`/`.cmesh`.

## Where things go at runtime

When the editor or Game.exe launches a project:

1. Loads `<project>.garden`.
2. Resolves `game_module` relative to the project folder, appends the platform suffix, copies it to a temp path, and loads it.
3. Calls `gardenGetAPIVersion` and `gardenRegisterComponents`.
4. Loads `default_level`, resolving GameMode from URL options, level override, then project defaults.
5. Calls `gardenGameInit(EngineServices*)` and `gardenOnLevelLoaded()`.
6. Starts ticking the world GameMode/GameState, then `gardenGameUpdate(dt)`.

See [Game Module](game-module.md) for the full lifecycle.
