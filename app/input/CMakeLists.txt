# input 模块 CMake
set(INPUT_SRC
    key_event.c
    capture_session.c
)
add_library(ocr_input STATIC ${INPUT_SRC})
target_link_libraries(ocr_input PUBLIC ocr_utils ocr_comm ocr_capture ocr_ai ocr_archive ${LIBJPEG_LIBRARIES})
target_include_directories(ocr_input PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
