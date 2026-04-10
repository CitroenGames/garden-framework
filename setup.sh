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
Actions=run;generate;

[Desktop Action run]
Name=Run Game
Exec=$INSTALL_DIR/garden run %f

[Desktop Action generate]
Name=Generate Project Files
Exec=$INSTALL_DIR/garden generate %f
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
        APP_NAME="Garden Opener"
        APP_DIR="$HOME/Applications/${APP_NAME}.app"
        BUNDLE_ID="com.garden-engine.opener"
        GARDEN_CLI="$INSTALL_DIR/garden"
        PLIST_BUDDY="/usr/libexec/PlistBuddy"
        LSREGISTER="/System/Library/Frameworks/CoreServices.framework/Versions/A/Frameworks/LaunchServices.framework/Versions/A/Support/lsregister"

        if ! command -v osacompile &>/dev/null; then
            echo "  WARNING: osacompile not found. Cannot create .app bundle."
            echo "  Install Xcode Command Line Tools: xcode-select --install"
            echo "  For now, use 'garden open <file.garden>' from the terminal."
        else
            mkdir -p "$HOME/Applications"

            if [ -d "$APP_DIR" ]; then
                echo "  Updating existing ${APP_NAME}.app..."
                rm -rf "$APP_DIR"
            else
                echo "  Creating ${APP_NAME}.app..."
            fi

            # Write AppleScript source (expand $GARDEN_CLI to absolute path)
            APPLESCRIPT_SRC=$(mktemp /tmp/garden_opener.XXXXXX.applescript)
            cat > "$APPLESCRIPT_SRC" << APPLESCRIPT_EOF
on open theFiles
    repeat with aFile in theFiles
        set filePath to POSIX path of aFile
        do shell script "${GARDEN_CLI} open " & quoted form of filePath & " > /dev/null 2>&1 &"
    end repeat
end open

on run
    display dialog "Garden Opener" & return & return & "Double-click a .garden file in Finder to open it in the editor." buttons {"OK"} default button "OK" with title "Garden Engine"
end run
APPLESCRIPT_EOF

            osacompile -o "$APP_DIR" "$APPLESCRIPT_SRC"
            rm -f "$APPLESCRIPT_SRC"

            # Patch Info.plist with file type declarations
            PLIST="$APP_DIR/Contents/Info.plist"

            $PLIST_BUDDY -c "Add :CFBundleIdentifier string ${BUNDLE_ID}" "$PLIST" 2>/dev/null || \
                $PLIST_BUDDY -c "Set :CFBundleIdentifier ${BUNDLE_ID}" "$PLIST"
            $PLIST_BUDDY -c "Set :CFBundleName ${APP_NAME}" "$PLIST" 2>/dev/null || \
                $PLIST_BUDDY -c "Add :CFBundleName string ${APP_NAME}" "$PLIST"

            # Remove existing entries so re-runs are clean
            $PLIST_BUDDY -c "Delete :UTExportedTypeDeclarations" "$PLIST" 2>/dev/null || true
            $PLIST_BUDDY -c "Delete :CFBundleDocumentTypes" "$PLIST" 2>/dev/null || true

            # Define the .garden UTI
            $PLIST_BUDDY -c "Add :UTExportedTypeDeclarations array" "$PLIST"
            $PLIST_BUDDY -c "Add :UTExportedTypeDeclarations:0 dict" "$PLIST"
            $PLIST_BUDDY -c "Add :UTExportedTypeDeclarations:0:UTTypeIdentifier string com.garden-engine.project" "$PLIST"
            $PLIST_BUDDY -c "Add :UTExportedTypeDeclarations:0:UTTypeDescription string Garden Project File" "$PLIST"
            $PLIST_BUDDY -c "Add :UTExportedTypeDeclarations:0:UTTypeConformsTo array" "$PLIST"
            $PLIST_BUDDY -c "Add :UTExportedTypeDeclarations:0:UTTypeConformsTo:0 string public.json" "$PLIST"
            $PLIST_BUDDY -c "Add :UTExportedTypeDeclarations:0:UTTypeConformsTo:1 string public.data" "$PLIST"
            $PLIST_BUDDY -c "Add :UTExportedTypeDeclarations:0:UTTypeTagSpecification dict" "$PLIST"
            $PLIST_BUDDY -c "Add :UTExportedTypeDeclarations:0:UTTypeTagSpecification:public.filename-extension array" "$PLIST"
            $PLIST_BUDDY -c "Add :UTExportedTypeDeclarations:0:UTTypeTagSpecification:public.filename-extension:0 string garden" "$PLIST"

            # Declare that this app opens .garden files
            $PLIST_BUDDY -c "Add :CFBundleDocumentTypes array" "$PLIST"
            $PLIST_BUDDY -c "Add :CFBundleDocumentTypes:0 dict" "$PLIST"
            $PLIST_BUDDY -c "Add :CFBundleDocumentTypes:0:CFBundleTypeName string Garden Project File" "$PLIST"
            $PLIST_BUDDY -c "Add :CFBundleDocumentTypes:0:CFBundleTypeRole string Editor" "$PLIST"
            $PLIST_BUDDY -c "Add :CFBundleDocumentTypes:0:LSHandlerRank string Owner" "$PLIST"
            $PLIST_BUDDY -c "Add :CFBundleDocumentTypes:0:LSItemContentTypes array" "$PLIST"
            $PLIST_BUDDY -c "Add :CFBundleDocumentTypes:0:LSItemContentTypes:0 string com.garden-engine.project" "$PLIST"
            $PLIST_BUDDY -c "Add :CFBundleDocumentTypes:0:CFBundleTypeExtensions array" "$PLIST"
            $PLIST_BUDDY -c "Add :CFBundleDocumentTypes:0:CFBundleTypeExtensions:0 string garden" "$PLIST"

            # Ad-hoc code sign (required on macOS Sequoia 15.1+)
            codesign --force --deep --sign - "$APP_DIR" 2>/dev/null && \
                echo "  Code signed app bundle." || \
                echo "  WARNING: Code signing failed. File associations may not work."

            # Register with LaunchServices
            if [ -x "$LSREGISTER" ]; then
                "$LSREGISTER" -kill -r -domain local -domain system -domain user 2>/dev/null || true
                "$LSREGISTER" -f -R "$APP_DIR" 2>/dev/null
                echo "  Registered with LaunchServices."
            fi

            # Refresh Finder to pick up new file associations
            killall Finder 2>/dev/null || true

            # Set as default handler for .garden files
            SWIFT_HELPER=$(mktemp /tmp/garden_setdefault.XXXXXX.swift)
            cat > "$SWIFT_HELPER" << 'SWIFT_EOF'
import AppKit
import UniformTypeIdentifiers

let args = CommandLine.arguments
guard args.count == 3 else { exit(1) }
let appURL = URL(fileURLWithPath: args[1])
let ext = args[2]
let sema = DispatchSemaphore(value: 0)
var exitCode: Int32 = 0
NSWorkspace.shared.setDefaultApplication(at: appURL, toOpenFileExtension: ext) { error in
    if error != nil { exitCode = 1 }
    sema.signal()
}
sema.wait()
exit(exitCode)
SWIFT_EOF
            if swiftc -o /tmp/garden_setdefault "$SWIFT_HELPER" -framework AppKit 2>/dev/null; then
                if /tmp/garden_setdefault "$APP_DIR" "garden" 2>/dev/null; then
                    echo "  Set as default handler for .garden files."
                else
                    echo "  WARNING: Could not set default handler automatically."
                    echo "  TIP: Right-click a .garden file in Finder, choose 'Get Info',"
                    echo "       change 'Open with' to '${APP_NAME}', then click 'Change All...'."
                fi
                rm -f /tmp/garden_setdefault
            elif command -v duti &>/dev/null; then
                duti -s "$BUNDLE_ID" .garden all 2>/dev/null
                echo "  Set as default handler for .garden files."
            else
                echo "  TIP: To set as default handler, right-click a .garden file in Finder,"
                echo "       choose 'Get Info', change 'Open with' to '${APP_NAME}', then"
                echo "       click 'Change All...'."
            fi
            rm -f "$SWIFT_HELPER"

            echo "  Installed ${APP_NAME}.app to ~/Applications/"
        fi
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
if [ "$(uname -s)" = "Darwin" ]; then
echo "  Double-click any .garden file in Finder to open it."
fi
echo ""
