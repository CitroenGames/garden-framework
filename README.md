# garden-opengl

## description

A simple 3D renderer written in C++ using OpenGL 1.3.

![screenshot](screen.png)

## how the project is organized

| script             | function                                                                                                                                                |
| ------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `playerEntity.hpp` | Class that manages the player by handling the input through a callback, updating the player position and camera.                                        |
| `gameObject.hpp`   | Unity knockoff entity. It's basically an object class in the game world, with position, rotation, and so on. Components can be attached to gameObjects. |
| `mesh.hpp`         | First component I've made. Attaches to a gameObject and keeps track of mesh data to be rendered.                                                        |
| `rigidbody.hpp`    | Physics component. Whenever attached to a gameObject, that gameObject will receive physics (just gravity for now).                                      |
| `world.hpp`        | Generic world class. It's meant to be static but I guess there aren't statics in C++. **Handles collisions and keeps track of all entities.**           |
| `camera.hpp`       | Simple camera class with some utility functions. It's managed by the player.                                                                            |
| `renderer.hpp`     | Drawing mesh logic.                                                                                                                                     |

## what's new in this fork

* Added support for loading `.obj` and `.gltf` models (previously models were hardcoded using an `obj2cpp` converter).
* Improved overall code structure.
* Replaced the old Makefile with a modern CMake build system.
* Separated the renderer logic behind a proper API (previously hardcoded directly into the game logic).

## license

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