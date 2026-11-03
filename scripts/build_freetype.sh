#!/usr/bin/env bash
# Builds the vendored FreeType as a static library into build/freetype.
#
# The system only has the FreeType runtime (libfreetype.so.6); there are no
# headers and pkg-config is absent, and package installation is unavailable on
# this machine. Vendoring the source also gives us the same library for the
# Windows and Android inference builds later.
#
# Only TrueType/OpenType rendering is needed, so zlib/png/bzip2/harfbuzz/brotli
# support is disabled: plain .ttf files do not need them.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="${ROOT}/third_party/freetype"
BUILD="${ROOT}/build/freetype"
JOBS="${JOBS:-3}"

mkdir -p "${BUILD}"

cmake -S "${SRC}" -B "${BUILD}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=OFF \
    -DFT_DISABLE_ZLIB=TRUE \
    -DFT_DISABLE_BZIP2=TRUE \
    -DFT_DISABLE_PNG=TRUE \
    -DFT_DISABLE_HARFBUZZ=TRUE \
    -DFT_DISABLE_BROTLI=TRUE

cmake --build "${BUILD}" -j "${JOBS}"

echo "FreeType built: ${BUILD}/libfreetype.a"
