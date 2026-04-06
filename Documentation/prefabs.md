# Prefab System

Prefabs are reusable entity templates stored as `.prefab` files. A prefab captures an entity's components, mesh, and collider so it can be spawned repeatedly in the editor or from C++ game code.

## Editor Workflow

### Saving a Prefab

1. Select an entity in the Scene Hierarchy
2. Right-click and choose **Save as Prefab**
3. Pick a save location (`.prefab` extension is added automatically)

The prefab captures all components, the mesh, mesh rendering properties, and collider.

### Spawning a Prefab

There are two ways to spawn a prefab in the editor:

- **Drag and drop**: Drag a `.prefab` file from the Content Browser onto the Viewport
- **Double-click**: Double-click a `.prefab` file in the Content Browser

The spawned entity gets a **Prefab Instance** component that records which prefab it came from. This appears as a read-only section in the Inspector.

### Visual Indicators

- Prefab instances show a **purple puzzle-piece icon** in the Scene Hierarchy
- `.prefab` files appear in **purple** in the Content Browser
- The Content Browser filter dropdown includes a **Prefabs** filter

## C++ API

### Header

```cpp
#include "Prefab/PrefabManager.hpp"
```

### Spawning

```cpp
// Spawn at the transform stored in the prefab
auto entity = PrefabManager::get().spawn(registry, "assets/prefabs/enemy.prefab");

// Spawn at a specific position
auto entity = PrefabManager::get().spawnAt(registry, "assets/prefabs/enemy.prefab", 10.0f, 0.0f, 5.0f);
```

### From a Game DLL

```cpp
#include "Prefab/PrefabManager.hpp"

static EngineServices* svc = nullptr;

GAME_API bool gardenGameInit(EngineServices* services)
{
    svc = services;
    return true;
}

GAME_API void gardenOnLevelLoaded()
{
    // Spawn enemies from prefab
    for (int i = 0; i < 5; i++)
    {
        PrefabManager::get().spawnAt(
            svc->game_world->registry,
            "assets/prefabs/enemy.prefab",
            i * 3.0f, 0.0f, 10.0f);
    }
}
```

### Saving from Code

```cpp
PrefabManager::get().savePrefab(
    registry,
    entity,
    "assets/prefabs/my_entity.prefab",
    "assets/models/mesh.glb",          // mesh path (optional)
    "assets/models/collider.obj");     // collider path (optional)
```

### Loading Without Spawning

```cpp
PrefabData data;
if (PrefabManager::loadPrefab("assets/prefabs/enemy.prefab", data))
{
    // data.name  — human-readable name
    // data.path  — file path
    // data.json  — full parsed JSON
}
```
