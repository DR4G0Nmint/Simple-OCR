add_executable(hello_crnn main.cpp)
target_link_libraries(hello_crnn PRIVATE NeoML NeoMathEngine)
set_target_properties(hello_crnn PROPERTIES
    BUILD_RPATH "${NEOML_BUILD_DIR}/src;${NEOML_BUILD_DIR}/NeoMathEngine/src")
