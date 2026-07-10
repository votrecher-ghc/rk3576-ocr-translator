# stab 模块 CMake
set(STAB_SRC
    imu_reader.c
    attitude_fusion.c
    motion_compensate.c
)
add_library(ocr_stab STATIC ${STAB_SRC})
target_link_libraries(ocr_stab PUBLIC ocr_utils ocr_comm ocr_isp)
target_include_directories(ocr_stab PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
