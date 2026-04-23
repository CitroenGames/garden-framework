# QuakeImporter

Reference editor plugin that imports assets from Quake 1 PAK archives. Serves
as a worked example of how to structure a foreign-game asset importer for
garden-framework.

## What it demonstrates

- **Archive parser** — `QuakePak.hpp/.cpp` reads `pak0.pak` / `pak1.pak`
  without loading the full file into memory. ~100 lines, no dependencies.
- **Import panel** — `QuakeImportPanel` registers itself with
  `EditorPanelRegistry`, shows a tree view grouped by asset kind
  (Models/Textures/Levels/Sounds), and extracts selected entries to
  `<project>/assets/imported/quake/`.
- **Background extraction** — `EditorServices::run_background` dispatches
  file-writes onto a worker so the editor frame stays responsive.
- **IAssetLoader stub** — `QuakeMdlLoader` registers against `AssetManager`
  with `getSourceId() = "QuakeImporter"` so the host can evict it on unload
  via `AssetManager::unregisterLoadersFromSource()`.
- **Menu integration** — a `File/Import/Quake PAK...` item opens the panel
  even when it's been closed.

## Build

```
garden generate-plugin QuakeImporter.gardenplugin
```

Drops `QuakeImporter.dll` AND `QuakeImporter.gardenplugin` into
`<engine>/plugins/`. Launch the editor — the plugin host picks them up at
startup. Confirm via `View → Plugin Manager`.

(Direct `sighmake QuakeImporter.buildscript -D ENGINE_PATH=../..` still works
for the build step but won't deploy the manifest, so the editor will load the
plugin with default metadata — name from the DLL stem, no version, etc.)

## Running

1. Open a project in the editor.
2. `File → Import → Quake PAK...` (the plugin's menu entry).
3. Paste a path to a Quake 1 `pak0.pak` / `pak1.pak` and click **Open**.
4. Tick entries (or use **Select All**) and **Extract Selected**.
5. Output lands under `<project>/assets/imported/quake/`. The asset scanner
   picks new files up on the next rescan; the Content Browser shows them.

## Extending to actual format conversion

`QuakeMdlLoader::loadFromFile` is intentionally a stub. To parse MDL into
`MeshAssetData`:

1. Read the `IDPO` magic + header (`numverts`, `numtris`, `numskins`,
   `numframes`, etc.).
2. Decode the embedded 8-bit-palettized skin (translate via the Quake
   palette to RGBA8 — palette is public domain).
3. Read triangle + vertex tables, de-quantize the 8-bit vertex positions
   using `scale` and `translate` from the header.
4. Emit a `MeshAssetData` (and optionally a `TextureAssetData`) inside a
   `LoadResult{ .success = true, .data = ... }`.

The `AssetManager` takes care of caching, GPU upload scheduling, and the
progress callback once `loadFromFile` returns a successful result.

## Legal note

PAK files are copyrighted id Software content. This plugin parses the format
— you must own the game to legally use the data.
