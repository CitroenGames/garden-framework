# Getting Started

This page walks you from a clean checkout to a running game DLL. If you only want to **build the engine itself**, see `../Documentation/build.md` — this page is about creating *your* game project on top of it.

## 1. Build the engine once

You need `Editor.exe`, `Game.exe`, `Server.exe`, and the two engine DLLs in `bin/`. From the engine root:

**Windows**

```bat
Generate SLN.bat
:: open build\Garden_.slnx in Visual Studio and build x64 Debug
compile_shaders_slang.bat
```

**Linux / macOS**

```bash
./Generate\ Makefile.sh
make -C build Release -j"$(nproc)"
./compile_shaders_slang.sh
```

`Editor.exe` should now launch and show the project selector. If shaders are missing, re-run the `compile_shaders_slang` script.

## 2. Install GardenCLI (optional but recommended)

```bat
install.bat            :: Windows
./install.sh           :: Linux / macOS
```

This puts a `garden` command on your `PATH`, registers `.garden` and `.gardenplugin` file associations, and registers the current engine checkout as an installed engine. After install:

```bash
garden list-engines
```

should show your checkout. The CLI is how you'll generate, build, and launch projects without touching Visual Studio.

## 3. Create a project

Two paths — pick one.

### A. From the editor

1. Run `bin\Editor.exe`.
2. In the project browser, click **New Project**.
3. Choose `EmptyProject`, `ThirdPerson`, or `FPSShooter`, pick a folder, name it.
4. The editor generates the project, opens it, and creates a default level.

### B. From the command line

Copy a template and point it at the engine:

```bat
xcopy /E /I Templates\EmptyProject C:\dev\MyGame
cd C:\dev\MyGame
sighmake EmptyProject.buildscript -D ENGINE_PATH=I:\github\garden-framework
```

Rename `EmptyProject.buildscript` and `EmptyProject.garden` if you want a different project name (also update `[project:...]` and `name` fields inside them).

## 4. Build the game DLL

The game DLL is built by the same `sighmake` invocation. Open the generated solution / makefiles in `build/` and build the project named after your `.garden` file. Output lands in `bin/<YourProject>.dll` (or `.so`/`.dylib`).

A successful build leaves you with:

```
MyGame/
├── MyGame.garden          # project descriptor
├── MyGame.buildscript     # sighmake project
├── src/                   # C++ game logic (compiles into the DLL)
├── assets/                # levels, models, textures, shaders, prefabs
└── bin/
    └── MyGame.dll         # game module loaded by Editor/Game/Server
```

## 5. Run it

**In the editor (recommended for iteration)**

```bat
bin\Editor.exe --project C:\dev\MyGame\MyGame.garden
```

Open the default level and press the **Play** button on the toolbar to enter Play-In-Editor. The editor loads `MyGame.dll`, calls `gardenGameInit`, and starts ticking `gardenGameUpdate`.

**As a standalone client**

```bat
bin\Game.exe --project C:\dev\MyGame\MyGame.garden
```

**Connect to a server**

```bat
bin\Game.exe --project C:\dev\MyGame\MyGame.garden --connect 127.0.0.1 --port 7777
```

**Run a dedicated server**

```bat
bin\Server.exe --project C:\dev\MyGame\MyGame.garden --port 7777
```

`Server.exe` requires your DLL to export the optional server hooks — see [Dedicated Server](dedicated-server.md).

## 6. Iterate

The editor copies your DLL on load, so you can rebuild while the editor is running. Re-enter Play to pick up the new DLL. Asset and level changes are picked up the next time the level loads.

For shader iteration, re-run `compile_shaders_slang` after editing anything under `assets/shaders/slang/`.

## What to read next

- [Project Structure](project-structure.md) — what each file in your project means.
- [Game Module](game-module.md) — the DLL exports the engine calls.
- [ECS & Components](ecs-and-components.md) — making your game *do* things.
