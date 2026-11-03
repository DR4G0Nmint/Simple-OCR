cmake_minimum_required(VERSION 3.16)
project(OcrAI LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# NeoML is built separately by scripts/build_neoml.sh (it is vendored under
# third_party/neoml). We link against that prebuilt tree instead of adding it
# as a subdirectory, so that a clean build of our own code does not rebuild
# the ~100 NeoML translation units.
set(NEOML_BUILD_DIR "${CMAKE_SOURCE_DIR}/build/neoml" CACHE PATH "NeoML build tree")
set(NEOML_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/neoml" CACHE PATH "NeoML source tree")

if(NOT EXISTS "${NEOML_BUILD_DIR}/src/libNeoML.so")
    message(FATAL_ERROR "NeoML not built. Run scripts/build_neoml.sh first. Looked for ${NEOML_BUILD_DIR}/src/libNeoML.so")
endif()

# NeoML's FineObjLite headers are included as <FineObjLiteDefs.h>, so that
# directory must be on the include path as well.
set(NEOML_INCLUDE_DIRS
    "${NEOML_SOURCE_DIR}/NeoML/include"
    "${NEOML_SOURCE_DIR}/NeoML/include/NeoML/FineObjLite"
    "${NEOML_SOURCE_DIR}/NeoMathEngine/include")

add_library(NeoMathEngine SHARED IMPORTED)
set_target_properties(NeoMathEngine PROPERTIES
    IMPORTED_LOCATION "${NEOML_BUILD_DIR}/NeoMathEngine/src/libNeoMathEngine.so"
    INTERFACE_INCLUDE_DIRECTORIES "${NEOML_INCLUDE_DIRS}")

# NeoML's headers select the platform code path via the _LINUX / _FINAL macros
# (its own CMake defines these for the library build, but they are also
# required by any translation unit that includes NeoML headers).
add_compile_definitions(_LINUX _FINAL)

add_library(NeoML SHARED IMPORTED)
set_target_properties(NeoML PROPERTIES
    IMPORTED_LOCATION "${NEOML_BUILD_DIR}/src/libNeoML.so"
    INTERFACE_INCLUDE_DIRECTORIES "${NEOML_INCLUDE_DIRS}"
    INTERFACE_LINK_LIBRARIES NeoMathEngine)

# FreeType, built by scripts/build_freetype.sh
set(FREETYPE_BUILD_DIR "${CMAKE_SOURCE_DIR}/build/freetype" CACHE PATH "FreeType build tree")
set(FREETYPE_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/freetype" CACHE PATH "FreeType source tree")

if(NOT EXISTS "${FREETYPE_BUILD_DIR}/libfreetype.a")
    message(FATAL_ERROR "FreeType not built. Run scripts/build_freetype.sh first. Looked for ${FREETYPE_BUILD_DIR}/libfreetype.a")
endif()

add_library(freetype STATIC IMPORTED)
set_target_properties(freetype PROPERTIES
    IMPORTED_LOCATION "${FREETYPE_BUILD_DIR}/libfreetype.a"
    INTERFACE_INCLUDE_DIRECTORIES "${FREETYPE_SOURCE_DIR}/include")
target_compile_definitions(freetype INTERFACE FT2_BUILD_LIBRARY)
target_compile_definitions(freetype INTERFACE FT_CONFIG_OPTION_USE_MODULE_ERRORS)

add_subdirectory(tools/hello_crnn)
add_subdirectory(src/generator)
add_subdirectory(tools)