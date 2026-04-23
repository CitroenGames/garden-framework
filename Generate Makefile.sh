#!/bin/bash
cd "$(dirname "$0")"

# Pick the platform-appropriate sighmake binary once.
if [ "$(uname)" = "Darwin" ]; then
    SIGHMAKE=./sighmake_macos
else
    SIGHMAKE=./sighmake
fi

# Main engine + tooling solution.
"$SIGHMAKE" project.buildscript -g makefile

# Bundled editor plugins. Each is a standalone sighmake solution that links
# against EngineSDK, so they're generated separately. Engine must be built
# before plugins because they pull in ../bin/EngineCore.lib etc.
mkdir -p plugins
for PLUGIN in EditorPlugins/QuakeImporter/QuakeImporter.buildscript; do
    if [ -f "$PLUGIN" ]; then
        echo "Generating bundled plugin: $PLUGIN"
        "$SIGHMAKE" "$PLUGIN" -D ENGINE_PATH=. -g makefile
    fi
done
