# pipeline 模块 CMake
set(PIPELINE_SRC
    buffer.c
    buffer_pool.c
    zerocopy.c
    node.c
    pipeline.c
)
add_library(ocr_pipeline STATIC ${PIPELINE_SRC})
target_link_libraries(ocr_pipeline PUBLIC ocr_utils ocr_comm pthread)
target_include_directories(ocr_pipeline PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
