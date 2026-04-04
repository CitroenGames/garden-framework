# Building Garden Framework

This project uses **Sighmake** for build configuration.

## Prerequisites

### Windows
*   Visual Studio 2022 (MSVC)
*   Sighmake (included or in path)
*   Vulkan SDK

### Linux (Ubuntu)
*   GCC/Clang
*   Sighmake
*   Vulkan SDK - Install with:
    ```bash
    sudo rm /etc/apt/sources.list.d/lunarg-vulkan-focal.list
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

## How to Build

### Windows

**Option A: Visual Studio Solution**
1.  Run `Generate SLN.bat` to generate the Visual Studio Solution.
2.  Open the solution in Visual Studio.
3.  Build and Run (Release x64 recommended).

**Option B: Makefile**
1.  Run `Generate Makefile.bat` to generate build files.
2.  Run `make -C build` to build.

### Linux
1.  Run `./Generate Makefile.sh` to generate build files.
2.  Run `make -C build` to build.

### macOS
1.  Run `./Generate Makefile.sh` to generate build files.
2.  Run `make -C build` to build.

## Running

### Windows
```bash
Game.exe -vulkan   # Vulkan backend
Game.exe -d3d11    # Direct3D 11 backend
```

### Linux
```bash
./Game -vulkan
```

### macOS
```bash
./build/Debug/Game   # Uses Metal backend
```

## Shader Compilation

Vulkan shaders need to be compiled to SPIR-V before use:

*   **Windows**: Run `compile_shaders.bat`
*   **Linux/macOS**: Run `./compile_shaders.sh`

Metal shaders (macOS only):
*   Run `./compile_metal_shaders.sh`
