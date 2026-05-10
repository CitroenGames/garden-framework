# Building Garden Framework

Garden Framework uses [Sighmake](https://github.com/CitroenGames/sighmake) as the source of truth for project generation and command-line builds. The root `project.buildscript` includes the engine libraries, game/editor/server executables, GardenCLI, NetworkTests, and third-party libraries.

Generated project files are written to `build/`. Build outputs are written to `bin/`.

## Prerequisites

### All Platforms

* Sighmake available on `PATH`, or a platform-local Sighmake binary where the helper scripts expect it.
* A C++20-capable compiler toolchain.
* Vulkan SDK installed and discoverable by Sighmake. The build scripts currently call `find_package(Vulkan REQUIRED)` for the graphics module.
* The bundled Slang compiler under `Tools/slang-2026.5.2/` for D3D12/Vulkan shader compilation.

### Windows

* Visual Studio 2022 or newer with the Desktop development with C++ workload.
* Windows SDK and MSBuild. Use a Visual Studio Developer shell if `msbuild`, `cl`, or linker tools are not on `PATH`.
* Vulkan SDK with `VULKAN_SDK` set.
* Direct3D 12 support from the Windows SDK.

SDL3 is vendored for Windows. `Generate SLN.bat` copies `Engine/Thirdparty/SDL3-3.4.4/lib/x64/SDL3.dll` into `bin/`.

### Linux

* GCC 12+ or Clang 15+.
* `make`, `pkg-config`, and SDL3 development packages.
* Vulkan SDK.

Ubuntu/Debian example:

```bash
sudo apt install build-essential pkg-config libsdl3-dev
wget -qO- https://packages.lunarg.com/lunarg-signing-key-pub.asc | sudo tee /etc/apt/trusted.gpg.d/lunarg.asc
sudo wget -qO /etc/apt/sources.list.d/lunarg-vulkan-noble.list https://packages.lunarg.com/vulkan/lunarg-vulkan-noble.list
sudo apt update
sudo apt install vulkan-sdk
```

### macOS

* Xcode Command Line Tools: `xcode-select --install`
* Homebrew packages: `brew install sdl3 pkg-config`
* Vulkan SDK for the current build scripts.
* Xcode Metal tools if you need to rebuild the Metal shader library.

## Quick Start

### Windows Command Line

```bat
sighmake project.buildscript
sighmake --build . --config Debug --platform x64 --parallel 8
compile_shaders_slang.bat
bin\Editor.exe -d3d12
```

Use `--config Release` for an optimized build:

```bat
sighmake --build . --config Release --platform x64 --parallel 8
```

### Windows Visual Studio

```bat
Generate SLN.bat
```

This generates `build/Garden_.slnx`, copies `SDL3.dll` into `bin/`, and generates the bundled editor plugin solution for `EditorPlugins/QuakeImporter`.

Open `build/Garden_.slnx` in Visual Studio and build `Debug|x64` or `Release|x64`.

### Linux/macOS Makefile

If `sighmake` is on `PATH`, generate Makefiles directly:

```bash
sighmake project.buildscript -g makefile
make -C build Release -j"$(nproc)"
```

On macOS, replace `$(nproc)` with `$(sysctl -n hw.ncpu)`.

The helper script also generates bundled editor plugin projects:

```bash
./Generate\ Makefile.sh
make -C build Release -j"$(nproc)"
```

`Generate Makefile.sh` expects `./sighmake` on Linux and `./sighmake_macos` on macOS. If you installed Sighmake elsewhere, either run the direct command above or place the expected binary at the repository root.

## Main Build Targets

| Target | Output | Notes |
| :--- | :--- | :--- |
| `EngineCore` | `bin/EngineCore.dll` | ECS, physics, audio, assets, networking, navigation, scene/project systems |
| `EngineGraphics` | `bin/EngineGraphics.dll` | Vulkan, D3D12, Metal, render graph, ImGui, RmlUi, ImGuizmo |
| `Game` | `bin/Game.exe` | Standalone game client |
| `Editor` | `bin/Editor.exe` | Level editor and Play-In-Editor host |
| `Server` | `bin/Server.exe` | Headless dedicated server entry point |
| `GardenCLI` | `bin/GardenCLI.exe` | Project, engine, and plugin command-line tool |
| `NetworkTests` | `bin/NetworkTests.exe` | Focused network regression test executable |

On Linux and macOS, library and executable extensions follow the platform defaults instead of `.dll` and `.exe`.

The Windows build also places third-party static libraries and PDB files under `bin/`.

## Shader Compilation

Runtime shaders live in `assets/shaders/slang/` and are compiled into backend-specific runtime formats.

### D3D12 and Vulkan

Windows:

```bat
compile_shaders_slang.bat
```

Linux/macOS:

```bash
./compile_shaders_slang.sh
```

Outputs:

| Directory | Format | Backend |
| :--- | :--- | :--- |
| `assets/shaders/compiled/d3d12/` | DXIL (`*.dxil`) | Direct3D 12 |
| `assets/shaders/compiled/vulkan/` | SPIR-V (`*.spv`) | Vulkan |

Run this after changing files under `assets/shaders/slang/` or after pulling shader changes.

### Metal

Metal shaders are hand-written under `assets/shaders/compiled/metal/`. On macOS, build the Metal library with:

```bash
./compile_shaders_metal.sh
```

This produces `assets/shaders/compiled/metal/shaders.metallib`.

## Running

### Editor

```bat
bin\Editor.exe [backend] [--project path\to\project.garden]
```

If no project is passed, the editor opens the project selector. Backend flags override the saved editor backend for that launch:

| Flag | Backend | Platforms |
| :--- | :--- | :--- |
| `-d3d12`, `--d3d12`, `-dx12`, `--dx12` | Direct3D 12 | Windows |
| `-vulkan`, `--vulkan` | Vulkan | Windows, Linux, macOS |
| `-metal`, `--metal` | Metal | macOS |

The default backend is D3D12 on Windows, Metal on macOS, and Vulkan elsewhere. Editor settings are saved next to the executable in `bin/editorconfig.cfg`.

### Game Client

```bat
bin\Game.exe --project path\to\project.garden [backend]
```

Network client options:

```bat
bin\Game.exe --project path\to\project.garden --connect 127.0.0.1 --port 7777
```

If `--project` is omitted, the client looks for a `.garden` file in the parent directory of the executable. Client CVars are archived in `bin/config.cfg`; command-line backend flags take precedence for the current launch.

### Dedicated Server

```bat
bin\Server.exe --project path\to\project.garden --port 7777
```

The server entry point runs headless and does not open a render window.

### Network Tests

```bat
bin\NetworkTests.exe
```

Build and run this target after changes to `Engine/src/Network` or project networking integration.

## GardenCLI Setup

Run the platform setup script from the repository root:

Windows:

```bat
install.bat
```

Linux/macOS:

```bash
./install.sh
```

The setup scripts install `garden` to the user-local PATH location, register `.garden` files, register `.gardenplugin` files, and register this engine installation.

Useful commands:

```bash
garden register-engine --path <engine-dir>
garden list-engines
garden open <project.garden>
garden run <project.garden>
garden run-server <project.garden>
garden generate <project.garden>
garden set-engine <project.garden> <engine-id>
garden new-plugin <name>
garden generate-plugin <plugin.gardenplugin>
garden list-plugins
```

## Projects and Plugins

Garden project files are JSON `.garden` files. They reference a buildscript, default level, game module path, asset directories, and an engine registration.

Templates live under `Templates/`. To generate the third-person template manually:

```bat
cd Templates\ThirdPerson
sighmake ThirdPerson.buildscript -D ENGINE_PATH=../..
```

To generate the FPS shooter template manually:

```bat
cd Templates\FPSShooter
sighmake FPSShooter.buildscript -D ENGINE_PATH=../..
```

Editor plugins are built as separate Sighmake solutions that link against the engine SDK libraries in `bin/`. The bundled Quake importer plugin is generated by `Generate SLN.bat` or `Generate Makefile.sh`. Custom plugins can be created and built through GardenCLI:

```bash
garden new-plugin MyPlugin
garden generate-plugin MyPlugin.gardenplugin
```

## Troubleshooting

| Symptom | Check |
| :--- | :--- |
| `sighmake` is not found | Install Sighmake and put it on `PATH`, or use the helper-script binary names expected by this repo. |
| `Vulkan` package not found | Install the Vulkan SDK and ensure `VULKAN_SDK` is set in the shell used for generation/build. |
| `msbuild` is not found | Use a Visual Studio Developer shell or open `build/Garden_.slnx` in Visual Studio. |
| `SDL3.dll` is missing at runtime | Run `Generate SLN.bat` or copy `Engine/Thirdparty/SDL3-3.4.4/lib/x64/SDL3.dll` to `bin/`. |
| Renderer starts with missing shader errors | Run `compile_shaders_slang.bat` or `./compile_shaders_slang.sh`. |
| Buildscript changes do not show up in Visual Studio | Re-run `sighmake project.buildscript` or `Generate SLN.bat`. |

## Third-Party Dependencies

Most third-party code is vendored under `Engine/Thirdparty/`; platform SDKs remain external.

| Library | Version/Source | Purpose |
| :--- | :--- | :--- |
| SDL3 | 3.4.4 | Windowing, input, audio device |
| Vulkan SDK | External | Vulkan rendering and shader tooling |
| Direct3D 12 | Windows SDK | D3D12 rendering |
| vk-bootstrap | Vendored | Vulkan instance/device setup |
| VMA | Vendored | Vulkan memory allocation |
| Dear ImGui | Docking branch | Editor and debug UI |
| ImGuizmo | 1.92 | 3D transform gizmos |
| RmlUi | 6.2 | In-game HTML/CSS UI |
| FreeType | Vendored | Font rasterization |
| Jolt Physics | 5.5.0 | Rigid body physics |
| miniaudio | Vendored | Audio playback |
| ENet | Vendored | UDP networking |
| entt | Vendored | Entity Component System |
| spdlog | 1.15.3 | Logging |
| GLM | Vendored | Math library |
| Pakker | Vendored | JSON and asset package utilities |
| tinygltf | 2.9.6 | glTF/GLB model loading |
| tinyobjloader | Vendored | OBJ model loading |
| meshoptimizer | Vendored | Mesh LOD and optimization |
| Recast Navigation | Vendored | Navigation mesh generation/runtime |
| bc7enc | Vendored | BC texture compression |
