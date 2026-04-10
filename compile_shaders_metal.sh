#!/bin/bash
# Compile Metal shader sources (.metal) into a precompiled Metal library (.metallib)
# Requires Xcode command-line tools (xcrun metal, xcrun metallib)
# The .metal source files are hand-written MSL (not generated from Slang)
# because the MetalRenderAPI uses Metal-specific UBO layouts and bindings.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SHADER_DIR="$SCRIPT_DIR/assets/shaders/compiled/metal"
OUTPUT="$SHADER_DIR/shaders.metallib"

if ! xcrun --find metal &>/dev/null; then
    echo "ERROR: Xcode metal compiler not found. Install Xcode command-line tools."
    exit 1
fi

if [ ! -d "$SHADER_DIR" ]; then
    echo "ERROR: Shader directory not found: $SHADER_DIR"
    exit 1
fi

COMMON="$SHADER_DIR/common.metal"
if [ ! -f "$COMMON" ]; then
    echo "ERROR: common.metal not found in $SHADER_DIR"
    exit 1
fi

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

# Concatenate common.metal first, then all other shader files into one
# translation unit. Shared types/functions live in common.metal.
COMBINED="$TMPDIR/combined.metal"
echo "#include <metal_stdlib>" > "$COMBINED"
echo "using namespace metal;" >> "$COMBINED"
echo "" >> "$COMBINED"

# Add common definitions first
echo "Adding common.metal..."
echo "// === common.metal ===" >> "$COMBINED"
sed -e 's/#include <metal_stdlib>//' -e 's/using namespace metal;//' "$COMMON" >> "$COMBINED"
echo "" >> "$COMBINED"

# Add remaining shader files (skip common.metal)
for src in "$SHADER_DIR"/*.metal; do
    name="$(basename "$src")"
    [ "$name" = "common.metal" ] && continue
    echo "Adding $name..."
    echo "// === $name ===" >> "$COMBINED"
    sed -e 's/#include <metal_stdlib>//' -e 's/using namespace metal;//' "$src" >> "$COMBINED"
    echo "" >> "$COMBINED"
done

echo "Compiling combined shaders..."
if ! xcrun -sdk macosx metal -c "$COMBINED" -o "$TMPDIR/combined.air" 2>&1; then
    echo "ERROR: Metal shader compilation failed."
    exit 1
fi

echo "Linking into shaders.metallib..."
if ! xcrun -sdk macosx metallib "$TMPDIR/combined.air" -o "$OUTPUT" 2>&1; then
    echo "ERROR: metallib linking failed."
    exit 1
fi

echo "Success: $OUTPUT"
echo "All Metal shaders compiled successfully!"
