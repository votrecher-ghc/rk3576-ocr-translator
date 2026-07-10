# main 模块 CMake（最终可执行文件）
set(MAIN_SRC
    main.c
    system_manager.c
)

add_executable(ocr_translator ${MAIN_SRC})

# 链接所有模块
target_link_libraries(ocr_translator PRIVATE
    ocr_utils
    ocr_comm
    ocr_pipeline
    ocr_capture
    ocr_isp
    ocr_display
    ocr_ai
    ocr_stab
    ocr_sensors
    ocr_input
    ocr_archive
    pthread
    m
)
