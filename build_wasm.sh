#!/bin/bash
# Build script for Uracil WASM module

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build_wasm"
OUTPUT_DIR="${SCRIPT_DIR}/../adeno-site/app/public/uracil"

echo "Building Uracil WASM module..."
echo "Source: ${SCRIPT_DIR}"
echo "Build: ${BUILD_DIR}"
echo "Output: ${OUTPUT_DIR}"

# Create build directory
mkdir -p "${BUILD_DIR}"
mkdir -p "${OUTPUT_DIR}"

cd "${BUILD_DIR}"

# Configure with Emscripten
emcmake cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="${EMSDK}/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake" 2>/dev/null || {
    echo "Trying without toolchain file..."
    emcmake cmake .. -DCMAKE_BUILD_TYPE=Release
}

# Build
emmake make -j$(nproc) uracil_wasm_module

# Copy output files
cp "${BUILD_DIR}/uracil.js" "${OUTPUT_DIR}/"
cp "${BUILD_DIR}/uracil.wasm" "${OUTPUT_DIR}/"

echo "Build complete!"
echo "Output files:"
ls -la "${OUTPUT_DIR}/"