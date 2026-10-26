#!/bin/bash
# Compile all Slang shaders to DXIL and SPIR-V
# Requires slangc in Tools/slang-2026.5.2/bin/
#
# Note: pbr.slang and common.slang are modules imported by other shaders.
# They are NOT compiled standalone — slangc resolves them automatically.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SLANGC="$SCRIPT_DIR/Tools/slang-2026.5.2/bin/slangc"
SHADER_DIR="$SCRIPT_DIR/assets/shaders/slang"
OUT_D3D12="$SCRIPT_DIR/assets/shaders/compiled/d3d12"
OUT_VK="$SCRIPT_DIR/assets/shaders/compiled/vulkan"

if [ ! -x "$SLANGC" ]; then
    echo "ERROR: slangc not found at $SLANGC"
    exit 1
fi

mkdir -p "$OUT_D3D12" "$OUT_VK"

ERRORS=0

# Helper: compile a shader to DXIL (VS+PS) and SPIR-V (vert+frag)
compile() {
    local NAME="$1"
    local VS_ENTRY="$2"
    local PS_ENTRY="$3"

    "$SLANGC" "$SHADER_DIR/$NAME.slang" -DTARGET_HLSL -target dxil -profile sm_6_0 -entry "$VS_ENTRY" -stage vertex -o "$OUT_D3D12/${NAME}_vs.dxil" 2>/dev/null || ((ERRORS++))
    "$SLANGC" "$SHADER_DIR/$NAME.slang" -DTARGET_HLSL -target dxil -profile sm_6_0 -entry "$PS_ENTRY" -stage fragment -o "$OUT_D3D12/${NAME}_ps.dxil" 2>/dev/null || ((ERRORS++))
    "$SLANGC" "$SHADER_DIR/$NAME.slang" -DTARGET_SPIRV -target spirv -profile glsl_450 -entry "$VS_ENTRY" -stage vertex -o "$OUT_VK/$NAME.vert.spv" 2>/dev/null || ((ERRORS++))
    "$SLANGC" "$SHADER_DIR/$NAME.slang" -DTARGET_SPIRV -target spirv -profile glsl_450 -entry "$PS_ENTRY" -stage fragment -o "$OUT_VK/$NAME.frag.spv" 2>/dev/null || ((ERRORS++))
}

echo "Compiling shadow..."
compile shadow vertexMain fragmentMain
echo "Compiling sky..."
compile sky vertexMain fragmentMain
echo "Compiling fxaa..."
compile fxaa vertexMain fragmentMain
echo "Compiling basic (with PBR)..."
compile basic vertexMain fragmentMain
echo "Compiling unlit..."
compile unlit vertexMain fragmentMain
echo "Compiling skinned (with PBR)..."
compile skinned vertexMain fragmentMain
echo "Compiling skinned_shadow..."
compile skinned_shadow vertexMain fragmentMain
echo "Compiling shadow_alphatest..."
compile shadow_alphatest vertexMain fragmentMain
echo "Compiling ssao..."
compile ssao vertexMain fragmentMain
echo "Compiling ssao_blur..."
compile ssao_blur vertexMain fragmentMain
echo "Compiling shadow_mask..."
compile shadow_mask vertexMain fragmentMain

# rmlui has 3 entry points
echo "Compiling rmlui..."
"$SLANGC" "$SHADER_DIR/rmlui.slang" -DTARGET_HLSL -target dxil -profile sm_6_0 -entry vertexMain -stage vertex -o "$OUT_D3D12/rmlui_vs.dxil" 2>/dev/null || ((ERRORS++))
"$SLANGC" "$SHADER_DIR/rmlui.slang" -DTARGET_HLSL -target dxil -profile sm_6_0 -entry fragmentTextured -stage fragment -o "$OUT_D3D12/rmlui_ps_textured.dxil" 2>/dev/null || ((ERRORS++))
"$SLANGC" "$SHADER_DIR/rmlui.slang" -DTARGET_HLSL -target dxil -profile sm_6_0 -entry fragmentColor -stage fragment -o "$OUT_D3D12/rmlui_ps_color.dxil" 2>/dev/null || ((ERRORS++))
"$SLANGC" "$SHADER_DIR/rmlui.slang" -DTARGET_SPIRV -target spirv -profile glsl_450 -entry vertexMain -stage vertex -o "$OUT_VK/rmlui.vert.spv" 2>/dev/null || ((ERRORS++))
"$SLANGC" "$SHADER_DIR/rmlui.slang" -DTARGET_SPIRV -target spirv -profile glsl_450 -entry fragmentTextured -stage fragment -o "$OUT_VK/rmlui_texture.frag.spv" 2>/dev/null || ((ERRORS++))
"$SLANGC" "$SHADER_DIR/rmlui.slang" -DTARGET_SPIRV -target spirv -profile glsl_450 -entry fragmentColor -stage fragment -o "$OUT_VK/rmlui_color.frag.spv" 2>/dev/null || ((ERRORS++))

echo ""
if [ "$ERRORS" -eq 0 ]; then
    echo "All shaders compiled successfully!"
else
    echo "$ERRORS shader compilation(s) failed."
    exit 1
fi
