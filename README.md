# garden-opengl

## description
a simple 3d game renderer written in c++ using opengl 1.3.

![screenshot](screen.png)

## how the project is organized
|script| function |
|--|--|
| playerEntity.hpp | class that manages the player by handling the input through a callback, updating the player position and camera. |
| gameObject.hpp | unity knockoff entity. it's basically an object class in the gameworld, with position rotation and so on. components can be attached to gameObjects.
| mesh.hpp | first component I've made. attaches to a gameObject and keeps track of mesh data to be rendered.
| rigidbody.hpp | physics component. whenever attached to a gameObject that gameObject will receive physics (just gravity for now).
| world.hpp | generic world class. it's meant to be static but I guess there aren't statics in C++. **handles collisions and keeps track off all entities**.
| camera.hpp | simple camera class with some utility functions. it's managed by the player.
| renderer.hpp | drawing mesh logic.

## license
           DO WHAT THE FUCK YOU WANT TO PUBLIC LICENSE
                   Version 2, December 2004
 
Copyright (C) 2004 Sam Hocevar <sam@hocevar.net>

Everyone is permitted to copy and distribute verbatim or modified
copies of this license document, and changing it is allowed as long
as the name is changed.
 
           DO WHAT THE FUCK YOU WANT TO PUBLIC LICENSE
  TERMS AND CONDITIONS FOR COPYING, DISTRIBUTION AND MODIFICATION

 0. You just DO WHAT THE FUCK YOU WANT TO.

If you can credit me I would be glad but don't worry about it otherwise ;)
