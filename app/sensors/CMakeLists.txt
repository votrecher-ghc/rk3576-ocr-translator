# sensors 模块 CMake
set(SENSORS_SRC
    iio_discovery.c
    light_sensor.c
    brightness_ctrl.c
    temp_sensor.c
    fan_ctrl.c
)
add_library(ocr_sensors STATIC ${SENSORS_SRC})
target_link_libraries(ocr_sensors PUBLIC ocr_utils m)
target_include_directories(ocr_sensors PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
