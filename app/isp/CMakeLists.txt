# isp 模块 CMake
set(ISP_SRC
    rga_api.c
    rga_proc.c
    rga_stabilizer.c
)
add_library(ocr_isp STATIC ${ISP_SRC})
# rga_proc.h 引用了 capture 模块的 v4l2_capture.h, 需要其包含目录
target_link_libraries(ocr_isp PUBLIC ocr_utils ocr_pipeline ocr_capture
                                     ${LIBRGA_LIBRARIES})
target_include_directories(ocr_isp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
