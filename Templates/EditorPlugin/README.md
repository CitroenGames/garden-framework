# Editor Plugin Template

A minimum working editor plugin. Demonstrates:

- The required ABI entry points (`gardenEditorGetAPIVersion`,
  `gardenEditorGetPluginName`, `gardenEditorGetPluginVersion`,
  `gardenEditorInit`, `gardenEditorShutdown`).
- Registering an `IEditorPanel` that ImGui draws every frame.
- Registering a main-menu item (`Tools → Say Hello`) that logs through the
  plugin's log hook.

## Scaffold

```
garden new-plugin MyPlugin
```

Copies this template to `./MyPlugin/`, renames the manifest + buildscript,
and bakes in the engine_id of your registered engine.

## Build

```
garden generate-plugin MyPlugin.gardenplugin
```

This invokes sighmake with the right `ENGINE_PATH` AND deploys the
`.gardenplugin` next to the built DLL under `<engine>/plugins/`. Launch the
editor; the plugin host scans that directory at startup. A successful load
shows up in the Console panel and in `View → Plugin Manager`.

(If you'd rather skip GardenCLI and call sighmake directly:
`sighmake MyPlugin.buildscript -D ENGINE_PATH=<engine>` — but you'll need to
copy the `.gardenplugin` into `<engine>/plugins/` yourself for the editor to
read its metadata.)

## ABI Contract

Plugins link the `EditorSDK` interface target, which re-exports everything
`EngineSDK` offers plus the editor-specific headers. You get access to:

- `Plugin/EditorPluginAPI.h` — `EditorServices`, `GARDEN_EDITOR_PLUGIN_API_VERSION`
- `Plugin/IEditorPanel.h` — panel base class
- `Plugin/EditorPanelRegistry.hpp`, `Plugin/MenuRegistry.hpp`
- `Assets/AssetManager.hpp`, `Assets/IAssetLoader.hpp` — register custom
  importers via `AssetManager::registerLoader()` and override `getSourceId()`
  to return your plugin's name so the host can evict your loaders on unload.
- `imgui.h`, `imgui_internal.h` — for panel UI.

## Hot Reload

The host copies plugin DLLs to `plugins/.hot/<name>_hot_N.dll` before loading,
so the original file stays writable. In `View → Plugin Manager`, click
**Reload** to rebuild-and-reload without restarting the editor.

## .gardenplugin manifest

The single source of truth for a plugin's metadata. Lives in the source folder
next to the buildscript; `garden generate-plugin` copies it to
`<engine>/plugins/<output_dll>.gardenplugin` so the editor reads the same
file the build tool does.

```json
{
    "name":           "MyPlugin",
    "version":        "0.1.0",
    "engine_id":      "<engine_id>",
    "min_editor_api": 1,
    "buildscript":    "MyPlugin.buildscript",
    "output_dll":     "MyPlugin",
    "tags":           ["importer"],
    "enabled":        true
}
```

Required: `name`, `buildscript`. Set `enabled: false` to keep the file in
place but skip loading. Set `min_editor_api` higher than the running editor's
`GARDEN_EDITOR_PLUGIN_API_VERSION` to force a graceful refusal.
