#!/bin/bash
# Compile Metal shaders to a metallib

SHADER_DIR="assets/shaders/metal"
OUTPUT_DIR="assets/shaders/metal"

echo "Compiling Metal shaders..."

# Check for xcrun
if ! command -v xcrun &> /dev/null; then
    echo "xcrun not found. Install Xcode to compile Metal shaders."
    echo "Shaders will be compiled at runtime instead."
    exit 0
fi

# Compile each .metal file to .air
for shader in "$SHADER_DIR"/*.metal; do
    name=$(basename "$shader" .metal)
    echo "  Compiling $name.metal -> $name.air"
    xcrun -sdk macosx metal -c "$shader" -o "$OUTPUT_DIR/$name.air"
    if [ $? -ne 0 ]; then
        echo "Failed to compile $name.metal"
        exit 1
    fi
done

# Link all .air files into a single metallib
echo "  Linking -> shaders.metallib"
xcrun -sdk macosx metallib "$OUTPUT_DIR"/*.air -o "$OUTPUT_DIR/shaders.metallib"
if [ $? -ne 0 ]; then
    echo "Failed to link metallib"
    exit 1
fi

# Clean up .air files
rm -f "$OUTPUT_DIR"/*.air

echo "Metal shaders compiled successfully."
