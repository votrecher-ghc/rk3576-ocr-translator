# ai 模块 CMake
set(AI_SRC
    rknn_runtime.c
    ocr_det.c
    ocr_rec.c
    ocr_postproc.c
    translator.c
    tokenizer.c
    model_loader.c
)
add_library(ocr_ai STATIC ${AI_SRC})
target_link_libraries(ocr_ai PUBLIC ocr_utils ocr_pipeline ocr_isp ${RKNN_LIBRARY} ${CJSON_LIBRARIES})
target_include_directories(ocr_ai PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
