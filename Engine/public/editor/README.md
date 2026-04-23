# Editor Plugin SDK — Public Headers

Plugins should include the ABI via:

```cpp
#include "Plugin/EditorPluginAPI.h"      // EditorServices, version macro, export macro
#include "Plugin/IEditorPanel.h"          // base class for dynamic panels
#include "Plugin/EditorPanelRegistry.hpp" // registry.add(panel, name)
#include "Plugin/MenuRegistry.hpp"        // registry.add(path, callback, user, name)
#include "Assets/AssetManager.hpp"        // registerLoader with custom getSourceId()
#include "Assets/IAssetLoader.hpp"
```

These paths work because EngineSDK exposes `Engine/src/` as a public include
directory, and EditorSDK extends EngineSDK. This directory is reserved for
future editor-only headers that shouldn't ship in game builds.

See `Templates/EditorPlugin/` for a minimum working plugin.
