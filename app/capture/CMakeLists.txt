# capture 模块 CMake
set(CAPTURE_SRC
    v4l2_capture.c
    v4l2_dmabuf.c
    imx415_ctrl.c
)
add_library(ocr_capture STATIC ${CAPTURE_SRC})
target_link_libraries(ocr_capture PUBLIC ocr_utils ocr_pipeline)
target_include_directories(ocr_capture PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
