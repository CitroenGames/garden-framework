#!/bin/bash
# Compile all Slang shaders to DXIL and SPIR-V
# Uses slangc from Tools/slang-2026.20/bin/ — downloaded automatically
# from https://github.com/shader-slang/slang/releases/tag/v2026.20
# on first run if missing. Tools/slang-2026.20/ is git-ignored.
#
# Note: pbr.slang and common.slang are modules imported by other shaders.
# They are NOT compiled standalone — slangc resolves them automatically.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SLANG_VERSION="2026.20"
SLANG_TAG="v${SLANG_VERSION}"
SLANG_DIR="$SCRIPT_DIR/Tools/slang-$SLANG_VERSION"
SLANGC="$SLANG_DIR/bin/slangc"
SHADER_DIR="$SCRIPT_DIR/assets/shaders/slang"
OUT_D3D12="$SCRIPT_DIR/assets/shaders/compiled/d3d12"
OUT_VK="$SCRIPT_DIR/assets/shaders/compiled/vulkan"

# Download and extract Slang $SLANG_VERSION into $SLANG_DIR if slangc is
# missing. The release archive is flat (bin/, lib/, include/ at its root),
# so extracting it into $SLANG_DIR yields $SLANG_DIR/bin/slangc.
ensure_slang() {
    if [ -x "$SLANGC" ]; then
        return 0
    fi

    local os arch platform slang_arch asset url tmp stage root slangc_path
    os="$(uname -s)"
    arch="$(uname -m)"
    case "$os" in
        Linux)  platform="linux" ;;
        Darwin) platform="macos" ;;
        *)
            echo "ERROR: Unsupported OS '$os' for automatic Slang download." >&2
            echo "Manually extract a Slang $SLANG_VERSION release into $SLANG_DIR" >&2
            echo "from https://github.com/shader-slang/slang/releases/tag/$SLANG_TAG" >&2
            return 1
            ;;
    esac
    case "$arch" in
        x86_64|amd64)   slang_arch="x86_64" ;;
        arm64|aarch64)  slang_arch="aarch64" ;;
        *)
            echo "ERROR: Unsupported architecture '$arch' for automatic Slang download." >&2
            return 1
            ;;
    esac

    asset="slang-${SLANG_VERSION}-${platform}-${slang_arch}.tar.gz"
    url="https://github.com/shader-slang/slang/releases/download/${SLANG_TAG}/${asset}"

    echo "Slang $SLANG_VERSION not found in $SLANG_DIR"
    echo "Downloading $url ..."
    tmp="$(mktemp -d)"
    stage="$tmp/extracted"
    mkdir -p "$stage"

    if command -v curl >/dev/null 2>&1; then
        if ! curl -L --fail --retry 3 -o "$tmp/$asset" "$url"; then
            echo "ERROR: Failed to download $url" >&2
            rm -rf "$tmp"
            return 1
        fi
    elif command -v wget >/dev/null 2>&1; then
        if ! wget -O "$tmp/$asset" "$url"; then
            echo "ERROR: Failed to download $url" >&2
            rm -rf "$tmp"
            return 1
        fi
    else
        echo "ERROR: Neither curl nor wget found; cannot download Slang." >&2
        rm -rf "$tmp"
        return 1
    fi

    echo "Extracting to $SLANG_DIR ..."
    if ! tar -xzf "$tmp/$asset" -C "$stage"; then
        echo "ERROR: Failed to extract $asset" >&2
        rm -rf "$tmp"
        return 1
    fi

    # Archive root normally contains bin/slangc directly; tolerate a
    # single nested top-level folder as well.
    root="$stage"
    if [ ! -f "$root/bin/slangc" ]; then
        slangc_path="$(find "$stage" -name slangc -type f -print -quit)"
        if [ -n "$slangc_path" ]; then
            root="$(cd "$(dirname "$slangc_path")/.." && pwd)"
        fi
    fi
    if [ ! -f "$root/bin/slangc" ]; then
        echo "ERROR: slangc not found in downloaded archive $asset" >&2
        rm -rf "$tmp"
        return 1
    fi

    mkdir -p "$SLANG_DIR"
    cp -R "$root/." "$SLANG_DIR/"
    rm -rf "$tmp"

    # tar preserves permissions, but make sure the tools are executable
    # in case the archive was extracted with a restrictive umask.
    chmod +x "$SLANG_DIR/bin/slangc" "$SLANG_DIR/bin/slang" \
        "$SLANG_DIR/bin/slangd" "$SLANG_DIR/bin/slangi" 2>/dev/null || true

    if [ -x "$SLANGC" ]; then
        echo "Slang $SLANG_VERSION installed to $SLANG_DIR"
    else
        echo "ERROR: Slang extraction did not produce $SLANGC" >&2
        return 1
    fi
}

ensure_slang

if [ ! -x "$SLANGC" ]; then
    echo "ERROR: slangc not found at $SLANGC"
    exit 1
fi

mkdir -p "$OUT_D3D12" "$OUT_VK"

ERRORS=0

# Suppress binding-overlap warning (E39001): Vulkan engine uses COMBINED_IMAGE_SAMPLER
# so texture+sampler sharing a vk::binding is intentional, not an error.
SLANG_WARN="-warnings-disable 39001"

# Helper: compile a shader to DXIL (VS+PS) and SPIR-V (vert+frag)
compile() {
    local NAME="$1"
    local VS_ENTRY="$2"
    local PS_ENTRY="$3"

    if [ ! -f "$SHADER_DIR/$NAME.slang" ]; then
        echo "  ERROR: Source file not found: $SHADER_DIR/$NAME.slang"
        ERRORS=$((ERRORS + 4))
        return 0
    fi

    "$SLANGC" $SLANG_WARN "$SHADER_DIR/$NAME.slang" -DTARGET_HLSL -target dxil -profile sm_6_0 -entry "$VS_ENTRY" -stage vertex -o "$OUT_D3D12/${NAME}_vs.dxil" 2>/dev/null || ERRORS=$((ERRORS + 1))
    "$SLANGC" $SLANG_WARN "$SHADER_DIR/$NAME.slang" -DTARGET_HLSL -target dxil -profile sm_6_0 -entry "$PS_ENTRY" -stage fragment -o "$OUT_D3D12/${NAME}_ps.dxil" 2>/dev/null || ERRORS=$((ERRORS + 1))
    "$SLANGC" $SLANG_WARN "$SHADER_DIR/$NAME.slang" -DTARGET_SPIRV -target spirv -profile glsl_450 -entry "$VS_ENTRY" -stage vertex -o "$OUT_VK/$NAME.vert.spv" 2>/dev/null || ERRORS=$((ERRORS + 1))
    "$SLANGC" $SLANG_WARN "$SHADER_DIR/$NAME.slang" -DTARGET_SPIRV -target spirv -profile glsl_450 -entry "$PS_ENTRY" -stage fragment -o "$OUT_VK/$NAME.frag.spv" 2>/dev/null || ERRORS=$((ERRORS + 1))
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
if [ ! -f "$SHADER_DIR/rmlui.slang" ]; then
    echo "  ERROR: Source file not found: $SHADER_DIR/rmlui.slang"
    ERRORS=$((ERRORS + 6))
else
    "$SLANGC" $SLANG_WARN "$SHADER_DIR/rmlui.slang" -DTARGET_HLSL -target dxil -profile sm_6_0 -entry vertexMain -stage vertex -o "$OUT_D3D12/rmlui_vs.dxil" 2>/dev/null || ERRORS=$((ERRORS + 1))
    "$SLANGC" $SLANG_WARN "$SHADER_DIR/rmlui.slang" -DTARGET_HLSL -target dxil -profile sm_6_0 -entry fragmentTextured -stage fragment -o "$OUT_D3D12/rmlui_ps_textured.dxil" 2>/dev/null || ERRORS=$((ERRORS + 1))
    "$SLANGC" $SLANG_WARN "$SHADER_DIR/rmlui.slang" -DTARGET_HLSL -target dxil -profile sm_6_0 -entry fragmentColor -stage fragment -o "$OUT_D3D12/rmlui_ps_color.dxil" 2>/dev/null || ERRORS=$((ERRORS + 1))
    "$SLANGC" $SLANG_WARN "$SHADER_DIR/rmlui.slang" -DTARGET_SPIRV -target spirv -profile glsl_450 -entry vertexMain -stage vertex -o "$OUT_VK/rmlui.vert.spv" 2>/dev/null || ERRORS=$((ERRORS + 1))
    "$SLANGC" $SLANG_WARN "$SHADER_DIR/rmlui.slang" -DTARGET_SPIRV -target spirv -profile glsl_450 -entry fragmentTextured -stage fragment -o "$OUT_VK/rmlui_texture.frag.spv" 2>/dev/null || ERRORS=$((ERRORS + 1))
    "$SLANGC" $SLANG_WARN "$SHADER_DIR/rmlui.slang" -DTARGET_SPIRV -target spirv -profile glsl_450 -entry fragmentColor -stage fragment -o "$OUT_VK/rmlui_color.frag.spv" 2>/dev/null || ERRORS=$((ERRORS + 1))
fi

echo ""
if [ "$ERRORS" -eq 0 ]; then
    echo "All shaders compiled successfully!"
else
    echo "$ERRORS shader compilation(s) failed."
    exit 1
fi
