#!/bin/bash
set -e

GARDEN_ROOT="$(cd "$(dirname "$0")" && pwd)"
CLI_SRC="$GARDEN_ROOT/Tools/GardenCLI"
INSTALL_DIR="$HOME/.local/bin"
BUILT_CLI=""

echo "============================================"
echo " Garden Engine Setup"
echo "============================================"
echo ""

# ---- Step 1: Build GardenCLI ----
echo "[1/5] Building Garden CLI tool..."

# Check if already built
if [ -f "$GARDEN_ROOT/bin/GardenCLI" ]; then
    echo "  Found existing build at bin/GardenCLI"
    BUILT_CLI="$GARDEN_ROOT/bin/GardenCLI"
elif [ -f "$GARDEN_ROOT/build/Release/GardenCLI" ]; then
    echo "  Found existing build at build/Release/GardenCLI"
    BUILT_CLI="$GARDEN_ROOT/build/Release/GardenCLI"
elif [ -f "$GARDEN_ROOT/GardenCLI" ]; then
    echo "  Found existing build at GardenCLI"
    BUILT_CLI="$GARDEN_ROOT/GardenCLI"
fi

if [ -z "$BUILT_CLI" ]; then
    # Try Sighmake
    if command -v sighmake &>/dev/null; then
        echo "  Using Sighmake..."
        cd "$GARDEN_ROOT"
        sighmake project.buildscript -g makefile
        NCPU=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
        make -C build Release -j"$NCPU" 2>/dev/null || true
        if [ -f "$GARDEN_ROOT/build/Release/GardenCLI" ]; then
            BUILT_CLI="$GARDEN_ROOT/build/Release/GardenCLI"
        fi
    fi

    # Fallback: direct g++/clang++ compilation
    if [ -z "$BUILT_CLI" ]; then
        CXX="${CXX:-g++}"
        if ! command -v "$CXX" &>/dev/null; then
            CXX="clang++"
        fi
        if command -v "$CXX" &>/dev/null; then
            echo "  Using direct compilation ($CXX)..."
            mkdir -p "$GARDEN_ROOT/build/cli_tmp"
            $CXX -std=c++20 -O2 \
                -I"$CLI_SRC/src" \
                -I"$GARDEN_ROOT/Engine/Thirdparty/tinygltf-2.9.6" \
                "$CLI_SRC/src/main.cpp" \
                "$CLI_SRC/src/EngineRegistry.cpp" \
                "$CLI_SRC/src/ProjectFile.cpp" \
                -o "$GARDEN_ROOT/build/cli_tmp/garden"
            BUILT_CLI="$GARDEN_ROOT/build/cli_tmp/garden"
        else
            echo "  ERROR: No C++ compiler found (g++, clang++)."
            echo "  Install a compiler and try again."
            exit 1
        fi
    fi
fi

if [ -z "$BUILT_CLI" ] || [ ! -f "$BUILT_CLI" ]; then
    echo "  ERROR: Build failed. No GardenCLI binary found."
    exit 1
fi

# ---- Step 2: Install ----
echo ""
echo "[2/5] Installing to $INSTALL_DIR..."

mkdir -p "$INSTALL_DIR"
rm -f "$INSTALL_DIR/garden"
cp "$BUILT_CLI" "$INSTALL_DIR/garden"
chmod +x "$INSTALL_DIR/garden"
echo "  Installed garden"

# ---- Step 3: Check PATH ----
echo ""
echo "[3/5] Checking PATH..."

if echo "$PATH" | tr ':' '\n' | grep -qx "$INSTALL_DIR"; then
    echo "  Already in PATH."
else
    echo "  WARNING: $INSTALL_DIR is not in your PATH."
    echo "  Add this line to your shell profile (~/.bashrc, ~/.zshrc, etc.):"
    echo ""
    echo "    export PATH=\"\$HOME/.local/bin:\$PATH\""
    echo ""
fi

# ---- Step 4: File association ----
echo ""
echo "[4/5] Setting up .garden file association..."

OS="$(uname -s)"
case "$OS" in
    Linux)
        DESKTOP_DIR="$HOME/.local/share/applications"
        MIME_DIR="$HOME/.local/share/mime/packages"
        mkdir -p "$DESKTOP_DIR" "$MIME_DIR"

        cat > "$DESKTOP_DIR/garden-engine.desktop" << DESKTOP_EOF
[Desktop Entry]
Type=Application
Name=Garden Engine
Comment=Open Garden project files
Exec=$INSTALL_DIR/garden %f
MimeType=application/x-garden-project;
NoDisplay=true
DESKTOP_EOF

        cat > "$MIME_DIR/garden-project.xml" << MIME_EOF
<?xml version="1.0"?>
<mime-info xmlns="http://www.freedesktop.org/standards/shared-mime-info">
  <mime-type type="application/x-garden-project">
    <comment>Garden Project File</comment>
    <glob pattern="*.garden"/>
  </mime-type>
</mime-info>
MIME_EOF

        update-mime-database "$HOME/.local/share/mime" 2>/dev/null || true
        xdg-mime default garden-engine.desktop application/x-garden-project 2>/dev/null || true
        update-desktop-database "$DESKTOP_DIR" 2>/dev/null || true
        echo "  Registered .garden MIME type and desktop entry."
        ;;
    Darwin)
        echo "  macOS: File association requires an .app bundle."
        echo "  For now, use 'garden open <file.garden>' from the terminal."
        ;;
    *)
        echo "  Unknown OS: $OS. Skipping file association."
        ;;
esac

# ---- Step 5: Register engine ----
echo ""
echo "[5/5] Registering engine..."

"$INSTALL_DIR/garden" register-engine --path "$GARDEN_ROOT"

echo ""
echo "============================================"
echo " Setup complete!"
echo "============================================"
echo ""
echo "Usage:"
echo "  garden list-engines              List registered engines"
echo "  garden open <file.garden>        Open a project"
echo "  garden set-engine <file> <id>    Link project to engine"
echo ""
