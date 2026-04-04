# Building Garden Framework

This project uses [Sighmake](https://github.com/CitroenGames/sighmake) for build configuration. Sighmake generates Visual Studio solutions on Windows and Makefiles on Linux/macOS from `.buildscript` files.

## Prerequisites

### Windows
*   Visual Studio 2022+ with the C++ desktop workload
*   [Vulkan SDK](https://vulkan.lunarg.com/) 1.2 or later (set `VULKAN_SDK` environment variable)
*   Sighmake on your PATH

### Linux (Ubuntu/Debian)
*   GCC 12+ or Clang 15+ with C++20 support
*   SDL2 development libraries (`sudo apt install libsdl2-dev`)
*   Sighmake on your PATH
*   Vulkan SDK:
    ```bash
    wget -qO- https://packages.lunarg.com/lunarg-signing-key-pub.asc | sudo tee /etc/apt/trusted.gpg.d/lunarg.asc
    sudo wget -qO /etc/apt/sources.list.d/lunarg-vulkan-noble.list https://packages.lunarg.com/vulkan/lunarg-vulkan-noble.list
    sudo apt update
    sudo apt install vulkan-sdk
    ```

### macOS
*   Xcode Command Line Tools (`xcode-select --install`)
*   Homebrew packages:
    ```bash
    brew install sdl2 pkg-config
    ```
*   [Vulkan SDK](https://vulkan.lunarg.com/) (for Vulkan backend) or use Metal natively

## Quick Start (Windows)

```bash
# 1. Generate project files
sighmake project.buildscript

# 2. Build (Release x64)
sighmake --build . --config Release

# 3. Compile Vulkan shaders to SPIR-V
compile_shaders.bat

# 4. Run
bin\Editor.exe -d3d11
```

Or use `Generate SLN.bat` to generate the solution, then open it in Visual Studio and build from there.

## Build Steps

### Generate Project Files

```bash
sighmake project.buildscript
```

This generates a Visual Studio solution (`.slnx`) on Windows or Makefiles on Linux/macOS. It also reads all `.buildscript` files under `Engine/`, `Game/`, and `Tools/`.

On Windows, `Generate SLN.bat` wraps this command and copies `SDL2.dll` to `bin/`.

### Build

**Command line (all platforms):**
```bash
sighmake --build . --config Release
```

**Visual Studio:** Open the generated solution and build with Release x64.

**Makefile (Linux/macOS):**
```bash
./Generate\ Makefile.sh
make -C build
```

### Build Outputs

All binaries are placed in `bin/`:

| Binary | Description |
| :--- | :--- |
| `EngineCore.dll` | Core engine: ECS, physics, audio, assets, networking |
| `EngineGraphics.dll` | Rendering: Vulkan, D3D11, Metal, ImGui, RmlUi |
| `Game.exe` | Standalone game client |
| `Editor.exe` | Level editor |
| `Server.exe` | Headless dedicated server |
| `GardenCLI.exe` | CLI tool for project management |
| `SDL2.dll` | Runtime dependency (Windows) |

## Shader Compilation

Vulkan shaders must be compiled to SPIR-V before running with the Vulkan backend. HLSL and Metal shaders are loaded from source at runtime.

**Windows:**
```bash
compile_shaders.bat
```
Requires `%VULKAN_SDK%\Bin\glslc.exe` or `glslc` on PATH.

**Linux/macOS:**
```bash
./compile_shaders.sh
```

**Metal (macOS only):**
```bash
./compile_metal_shaders.sh
```
Compiles `.metal` files to `shaders.metallib` using `xcrun`.

Shader sources are in `assets/shaders/`:

| Directory | Format | Backend |
| :--- | :--- | :--- |
| `assets/shaders/vulkan/` | GLSL &rarr; SPIR-V | Vulkan |
| `assets/shaders/d3d11/` | HLSL | Direct3D 11 |
| `assets/shaders/metal/` | Metal | Metal |

## Running

### Game Client

```bash
bin/Game.exe --project path/to/project.garden [backend]
```

If `--project` is not specified, the client looks for a `.garden` file in the parent directory of the executable.

**Backend flags (pick one):**

| Flag | Backend | Platforms |
| :--- | :--- | :--- |
| `-vulkan` / `--vulkan` | Vulkan | Windows, Linux, macOS |
| `-d3d11` / `--d3d11` / `-dx11` | Direct3D 11 | Windows |
| `-metal` / `--metal` | Metal | macOS |

Default: D3D11 on Windows, Vulkan on Linux, Metal on macOS.

### Editor

```bash
bin/Editor.exe [backend] [--project path/to/project.garden]
```

Same backend flags as the client. If no `--project` is given, the editor opens a project browser where you can create a new project from a template or open an existing one.

### Dedicated Server

```bash
bin/Server.exe --project path/to/project.garden
```

Always runs headless (no window, no GPU). Requires the project's game module to have server support.

## Setup & GardenCLI

Run `setup.bat` (Windows) or `setup.sh` (Linux/macOS) to:

1. Build the GardenCLI tool
2. Install `garden.exe` to your PATH (`%LOCALAPPDATA%\Garden\bin`)
3. Register `.garden` file associations (double-click to open in editor)
4. Register this engine installation

After setup, you can use the CLI:

```bash
garden open MyProject.garden          # Open project in editor
garden generate MyProject.garden      # Regenerate project build files
garden register-engine                # Register current directory as an engine
garden list-engines                   # Show registered engines
garden set-engine MyProject.garden <id>  # Link project to an engine
```

## Project Files

Garden uses `.garden` project files (JSON) that reference a buildscript, default level, game module path, and asset directories. The `Templates/` directory contains starter projects (e.g. `FPSShooter`) that can be used when creating a new project from the editor's project browser.

## Third-Party Dependencies

All dependencies are vendored under `Engine/Thirdparty/`:

| Library | Version | Purpose |
| :--- | :--- | :--- |
| SDL2 | 2.x | Windowing, input, audio device |
| Vulkan SDK | 1.2+ | Vulkan rendering |
| vk-bootstrap | | Vulkan instance/device setup |
| VMA | | Vulkan memory allocation |
| Dear ImGui | docking | Editor and debug UI |
| ImGuizmo | 1.92 | 3D transform gizmos |
| RmlUi | 6.2 | In-game HTML/CSS UI |
| FreeType | | Font rasterization (RmlUi) |
| Jolt Physics | 5.5.0 | Rigid body physics |
| miniaudio | | Spatial audio |
| entt | | Entity Component System |
| ENet | | UDP networking |
| spdlog | 1.15 | Logging |
| GLM | | Math library |
| tinygltf | 2.9.6 | glTF/GLB model loading |
| tinyobjloader | | OBJ model loading |
| meshoptimizer | | Mesh LOD generation |
| nlohmann/json | | JSON parsing (via Pakker) |
