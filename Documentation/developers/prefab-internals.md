# Prefab System Internals

Technical details for engine developers working on the prefab system.

## File Format

Prefabs are stored as JSON with the `.prefab` extension:

```json
{
    "format": "garden_prefab",
    "version": 1,
    "name": "Enemy Soldier",
    "components": {
        "TagComponent": { "name": "Enemy Soldier" },
        "TransformComponent": {
            "position": [0, 0, 0],
            "rotation": [0, 0, 0],
            "scale": [1, 1, 1]
        },
        "RigidBodyComponent": {
            "velocity": [0, 0, 0],
            "force": [0, 0, 0],
            "mass": 1.0,
            "apply_gravity": true
        }
    },
    "mesh": {
        "path": "assets/models/enemy.glb",
        "culling": true,
        "transparent": false,
        "visible": true,
        "casts_shadow": true,
        "force_lod": -1
    },
    "collider": {
        "mesh_path": "assets/models/enemy_collider.obj"
    }
}
```

### Format Details

- **`components`** uses the exact same format that `ReflectionSerializer::serializeEntity()` produces. No custom serialization is needed for reflected components.
- **`mesh`** and **`collider`** are top-level siblings of `components` because `MeshComponent` and `ColliderComponent` hold `shared_ptr<mesh>`, which is not reflectable. The mesh rendering properties (`culling`, `transparent`, `visible`, `casts_shadow`, `force_lod`) live on the `mesh` object itself, not on the component struct.
- Game DLL components that define a `reflect()` method are serialized automatically into the `components` block.
- `PrefabInstanceComponent` is stripped from the `components` block on save to avoid self-referencing loops.

## Architecture

### Key Files

| File | Purpose |
|------|---------|
| `Engine/src/Prefab/PrefabManager.hpp` | Singleton class declaration |
| `Engine/src/Prefab/PrefabManager.cpp` | Save, load, and spawn implementation |
| `Engine/src/Components/PrefabInstanceComponent.hpp` | Tag component tracking prefab origin |
| `Engine/src/Assets/AssetTypes.hpp` | `AssetType::Prefab` enum entry |
| `Engine/src/Reflection/EngineReflection.cpp` | Registers `PrefabInstanceComponent` |

### PrefabManager Design

`PrefabManager` is a Meyer's singleton (same pattern as `SceneManager`, `AssetManager`, `EventBus`). It is initialized once with `ReflectionRegistry*` and `IRenderAPI*`, then callers only need to pass a registry and prefab path:

```cpp
// Initialization (done once in Client main.cpp, Server main.cpp, and EditorApp.cpp)
PrefabManager::get().initialize(&reflection, render_api);

// Usage — no render_api or reflection needed
auto entity = PrefabManager::get().spawn(registry, "assets/prefabs/enemy.prefab");
```

Initialization happens after `registerEngineReflection()` and `game_module.registerComponents()` so that all component types (engine + game DLL) are registered before prefabs reference them.

### Save Flow

1. `ReflectionSerializer::serializeEntity()` produces `{"components": {...}}`
2. The `components` sub-object is extracted into the prefab JSON
3. `PrefabInstanceComponent` is removed from the components block
4. If a mesh path is provided, a `mesh` block is built with rendering properties read from the live `mesh` object
5. If a collider path is provided, a `collider` block is added
6. Written to disk as pretty-printed JSON

### Spawn Flow

1. File is read and parsed as JSON
2. Format and version are validated
3. `registry.create()` creates a new entity
4. `ReflectionSerializer::deserializeEntity()` restores all reflected components
5. If a `mesh` block exists, the mesh is loaded via `mesh(path, render_api)` constructor (using the stored `m_render_api`), rendering properties are applied, and `uploadToGPU()` is called (skipped when render_api is null)
6. If a `collider` block exists, the collider mesh is loaded similarly
7. `PrefabInstanceComponent` is emplaced with the prefab file path

### PrefabInstanceComponent

```cpp
struct PrefabInstanceComponent {
    std::string prefab_path;
};
```

- Reflected with `VisibleAnywhere` (read-only in inspector)
- `removable(false)` prevents users from stripping it
- Serialized into level files via the normal reflection pipeline
- Enables future features: override tracking, "revert to prefab", nested prefabs

### Editor Integration Points

| Location | What it does |
|----------|-------------|
| `SceneHierarchyPanel.cpp` | "Save as Prefab" context menu, purple puzzle icon for instances |
| `ContentBrowserPanel.cpp` | `.prefab` color/icon/filter, drag-drop source (`ASSET_PREFAB_PATH`), double-click spawn |
| `ViewportPanel.cpp` | Accepts `ASSET_PREFAB_PATH` drag-drop payload |
| `EditorApp.cpp` | Initializes PrefabManager, wires all callbacks |

### Mesh Path Cache

The editor maintains `InspectorPanel::mesh_path_cache` mapping `entt::entity -> std::string` because `MeshComponent` holds a `shared_ptr<mesh>` with no path stored on it. When spawning a prefab in the editor, the mesh path from the prefab JSON is inserted into this cache so that level save/serialize works correctly.

### Non-Reflectable Component Handling

`MeshComponent` and `ColliderComponent` cannot use the reflection system because they contain `shared_ptr<mesh>`. The prefab system handles them explicitly:

- **On save**: mesh/collider paths are passed as string parameters (sourced from the editor's `mesh_path_cache` and `LevelEntity` data)
- **On spawn**: paths are read from the JSON and meshes are loaded via the `mesh` constructor, matching the same pattern used by `LevelManager::loadMesh()` and `EditorApp::on_mesh_dropped`
