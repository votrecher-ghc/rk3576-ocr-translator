# display 模块 CMake
set(DISPLAY_SRC
    drm_device.c
    drm_plane.c
    drm_fb.c
    drm_overlay.c
)
add_library(ocr_display STATIC ${DISPLAY_SRC})
target_link_libraries(ocr_display PUBLIC ocr_utils ocr_pipeline ${LIBDRM_LIBRARIES} ${FREETYPE_LIBRARIES})
target_include_directories(ocr_display PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${LIBDRM_INCLUDE_DIRS})
