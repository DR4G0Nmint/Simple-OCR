#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"

echo "=== OCR-AI v1.0 Setup Script ==="
echo ""

# Step 1: Check if NeoML is already built
if [ ! -f "${BUILD_DIR}/neoml/src/libNeoML.so" ]; then
    echo "NeoML not found — building (first time may take a few minutes)..."
    bash "${PROJECT_DIR}/scripts/build_neoml.sh"
else
    echo "NeoML already built."
fi

# Step 2: Build the project if needed
if [ ! -f "${BUILD_DIR}/tools/hello_crnn/hello_crnn" ]; then
    echo "Building project..."
    cmake -S "${PROJECT_DIR}" -B "${BUILD_DIR}" -G Ninja
    cmake --build "${BUILD_DIR}" -j$(nproc)
else
    echo "Project already built."
fi

echo ""
echo "=== Available tools ==="
echo "1) hello_crnn  — NeoML smoke test (Step 1 verification)"
echo "    Usage: ${BUILD_DIR}/tools/hello_crnn/hello_crnn /tmp/test.model"
echo ""
echo "2) ocr_gen       — Synthetic data generator"
echo "    Usage: ${BUILD_DIR}/src/generator/ocr_gen --help"
echo ""

echo "=== Running hello_crnn smoke test ==="
"${BUILD_DIR}/tools/hello_crnn/hello_crnn" "/tmp/hello_crnn.model"
RESULT=$?
if [ $RESULT -eq 0 ]; then
    echo "✅ hello_crnn PASS — NeoML is working correctly."
else
    echo "❌ hello_crnn FAIL — check NeoML build."
fi

echo ""
echo "=== Done ==="
echo "Run './${0##*/}' again or use the individual binaries above."