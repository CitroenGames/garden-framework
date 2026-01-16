# garden-opengl

## Description

A 3D rendering engine and game testbed written in C++ using OpenGL 4.6.

![screenshot](screen.png)

## Features

*   **Modern OpenGL 4.6 Renderer**: Uses shaders, VAOs, and modern pipeline practices.
*   **Directional Lighting**: Configurable ambient, diffuse, and directional lighting.
*   **Data-Driven Levels**: Levels are loaded from JSON files (`levels/main.level.json`).
*   **Model Support**: Loads `.gltf`/`.glb` and `.obj` models.
*   **Entity Component System (ECS)**: Uses `entt` for efficient entity management.
*   **Physics**: Basic AABB collision and gravity.
*   **Input System**: Abstracted input handling supporting keyboard and mouse.

## Build System

This project uses **Sighmake** for build configuration.

### Prerequisites
*   Visual Studio 2022 (MSVC)
*   Sighmake (included or in path)

### How to Build
1.  Run `Generate SLN.bat` to generate the Visual Studio Solution (`OpenGL_.slnx` or `.sln`).
2.  Open the solution in Visual Studio.
3.  Build and Run (Release x64 recommended).

## Level Format (.json)

Levels are defined in JSON format. Example configuration for lighting:

```json
"lighting": {
  "ambient": { "r": 0.2, "g": 0.2, "b": 0.2 },
  "diffuse": { "r": 0.8, "g": 0.8, "b": 0.8 },
  "direction": { "x": 0.0, "y": -1.0, "z": -0.5 }
}
```

## Project Structure

| Component | Function |
| :--- | :--- |
| **Engine/** | Core engine code (Graphics, Input, Physics, ECS). |
| **Game/** | Game-specific logic and entry point. |
| **assets/** | Shaders (`.vert`, `.frag`) and other raw assets. |
| **levels/** | JSON level definitions. |
| **models/** | 3D models and textures. |

## License

```
           DO WHAT THE FUCK YOU WANT TO PUBLIC LICENSE
                   Version 2, December 2004

Copyright (C) 2004 Sam Hocevar <sam@hocevar.net>

Everyone is permitted to copy and distribute verbatim or modified
copies of this license document, and changing it is allowed as long
as the name is changed.

           DO WHAT THE FUCK YOU WANT TO PUBLIC LICENSE
  TERMS AND CONDITIONS FOR COPYING, DISTRIBUTION AND MODIFICATION

 0. You just DO WHAT THE FUCK YOU WANT TO.
```