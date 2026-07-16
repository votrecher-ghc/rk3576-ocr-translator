# display 模块 CMake
set(DISPLAY_SRC
    drm_device.c
    drm_plane.c
    drm_fb.c
    drm_sink.c
    drm_overlay.c
)

# pkg-config 不可用时也从 toolchain/sysroot 中定位 ft2build.h。
if(NOT FREETYPE_INCLUDE_DIRS)
    find_path(FREETYPE_FALLBACK_INCLUDE_DIR ft2build.h PATH_SUFFIXES freetype2)
    if(FREETYPE_FALLBACK_INCLUDE_DIR)
        set(FREETYPE_INCLUDE_DIRS ${FREETYPE_FALLBACK_INCLUDE_DIR})
    endif()
endif()

add_library(ocr_display STATIC ${DISPLAY_SRC})
target_link_libraries(ocr_display PUBLIC ocr_utils ocr_pipeline ${LIBDRM_LIBRARIES} ${FREETYPE_LIBRARIES})
target_include_directories(ocr_display PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LIBDRM_INCLUDE_DIRS}
    ${FREETYPE_INCLUDE_DIRS}
)
