#!/usr/bin/env bash
# Builds the vendored NeoML (CPU only) into build/neoml.
#
# Notes on the flags:
#  -DNeoOnnx_BUILD=OFF  NeoML's top-level CMake otherwise pulls protobuf via
#                       FetchContent from the network, which hangs on this
#                       machine. We do not need ONNX import, only training.
#  -DUSE_FINE_OBJECTS=OFF  No ABBYY FineObjects dependency.
#  -DNeoML_BUILD_TESTS=OFF Not needed, keeps build time down.
#  -include stdexcept    GCC 14 no longer transitively includes <stdexcept>;
#                       NeoML sources (MathEngine.cpp, CpuMathEngineDnnDistributed.cpp)
#                       use std::logic_error without including it. Rather than
#                       patch vendored files we force-include the header.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="${ROOT}/third_party/neoml/NeoML"
BUILD="${ROOT}/build/neoml"
PREFIX="${ROOT}/build/neoml-install"
JOBS="${JOBS:-3}"

mkdir -p "${BUILD}"

cmake -S "${SRC}" -B "${BUILD}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
    -DCMAKE_CXX_FLAGS="-include stdexcept" \
    -DUSE_FINE_OBJECTS=OFF \
    -DNeoOnnx_BUILD=OFF \
    -DNeoML_BUILD_TESTS=OFF

cmake --build "${BUILD}" -j "${JOBS}"

echo "NeoML built: ${BUILD}/src/libNeoML.so"
