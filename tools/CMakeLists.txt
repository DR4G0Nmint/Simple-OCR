# CRNN+CTC trainer
add_executable(crnn_trainer
    crnn_trainer.cpp
)
target_link_libraries(crnn_trainer PRIVATE NeoML NeoMathEngine)
set_target_properties(crnn_trainer PROPERTIES
    BUILD_RPATH "${NEOML_BUILD_DIR}/src;${NEOML_BUILD_DIR}/NeoMathEngine/src"
)

# OCR inference tool (loads .ocrbin model, runs greedy CTC decode)
add_executable(ocr_infer
    ocr_infer.cpp
)
target_link_libraries(ocr_infer PRIVATE NeoML NeoMathEngine)
set_target_properties(ocr_infer PROPERTIES
    BUILD_RPATH "${NEOML_BUILD_DIR}/src;${NEOML_BUILD_DIR}/NeoMathEngine/src"
)