#!/bin/bash
# Compile Vulkan shaders to SPIR-V
# Requires glslc from Vulkan SDK

set -e

# Find glslc - check VULKAN_SDK first, then PATH
if [ -n "$VULKAN_SDK" ]; then
    GLSLC="$VULKAN_SDK/bin/glslc"
elif command -v glslc &> /dev/null; then
    GLSLC="glslc"
else
    echo "ERROR: glslc not found. Install Vulkan SDK."
    exit 1
fi

echo "Using glslc: $GLSLC"

SHADER_DIR="assets/shaders/vulkan"

# Create output directory if it doesn't exist
mkdir -p "$SHADER_DIR"

for shader in basic shadow sky fxaa; do
    echo "Compiling $shader shaders..."
    "$GLSLC" "$SHADER_DIR/$shader.vert" -o "$SHADER_DIR/$shader.vert.spv"
    "$GLSLC" "$SHADER_DIR/$shader.frag" -o "$SHADER_DIR/$shader.frag.spv"
done

echo ""
echo "Shader compilation successful!"
