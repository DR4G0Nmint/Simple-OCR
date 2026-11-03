add_executable(ocr_gen
    main.cpp
    generator.cpp
    augment.cpp
    backgrounds.cpp
    fonts.cpp
    jpeg_artifacts.cpp
    renderer.cpp
    shard_writer.cpp
    stb_impl.cpp
    text.cpp
    verify.cpp
    ${CMAKE_SOURCE_DIR}/src/common/image.cpp
    ${CMAKE_SOURCE_DIR}/src/common/sha256.cpp)

target_include_directories(ocr_gen PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/stb)

target_link_libraries(ocr_gen PRIVATE freetype)

find_package(Threads REQUIRED)
target_link_libraries(ocr_gen PRIVATE Threads::Threads)

# FreeType is a C library; its headers are pulled in from inside C++.
target_compile_definitions(ocr_gen PRIVATE STBI_NO_HDR STBI_NO_LINEAR)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 10)
    target_compile_options(ocr_gen PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
