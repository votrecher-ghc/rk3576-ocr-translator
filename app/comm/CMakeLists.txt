# comm 模块 CMake
set(COMM_SRC
    msgbus.c
    event_loop.c
    ringbuffer.c
)
add_library(ocr_comm STATIC ${COMM_SRC})
target_link_libraries(ocr_comm PUBLIC ocr_utils pthread)
target_include_directories(ocr_comm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
