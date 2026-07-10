# archive 模块 CMake
set(ARCHIVE_SRC
    archiver.c
    storage_mgr.c
)
add_library(ocr_archive STATIC ${ARCHIVE_SRC})
target_link_libraries(ocr_archive PUBLIC ocr_utils ocr_comm ${CJSON_LIBRARIES})
target_include_directories(ocr_archive PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
