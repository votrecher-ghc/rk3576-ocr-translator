# utils 模块 CMake
set(UTILS_SRC
    log.c
    config.c
    timestamp.c
    math_utils.c
)

add_library(ocr_utils STATIC ${UTILS_SRC})
target_link_libraries(ocr_utils PUBLIC ${CJSON_LIBRARIES} pthread)
target_include_directories(ocr_utils PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
