# Unit tests are added from app/CMakeLists.txt so they link the exact same
# production targets as the application.

if(NOT TARGET ocr_utils OR NOT TARGET ocr_comm OR NOT TARGET ocr_pipeline)
    message(FATAL_ERROR
        "Tests must be configured through app/CMakeLists.txt so production targets exist")
endif()

function(ocr_add_unit_test target source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE ${ARGN})
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        # Several legacy tests use assert(); keep checks active in Release.
        target_compile_options(${target} PRIVATE -UNDEBUG)
    endif()
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES TIMEOUT 30)
endfunction()

ocr_add_unit_test(test_ringbuffer unit/test_ringbuffer.c ocr_comm)
ocr_add_unit_test(test_msgbus unit/test_msgbus.c ocr_comm)
ocr_add_unit_test(test_event_loop unit/test_event_loop.c ocr_comm)
ocr_add_unit_test(test_buffer unit/test_buffer.c ocr_pipeline pthread)
ocr_add_unit_test(test_config unit/test_config.c ocr_utils)
ocr_add_unit_test(test_pipeline integration/test_pipeline.c ocr_pipeline pthread)

add_executable(test_pose_history
    unit/test_pose_history.c
    "${CMAKE_CURRENT_LIST_DIR}/../app/stab/pose_history.c"
)
target_include_directories(test_pose_history PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../app/stab"
    "${CMAKE_CURRENT_LIST_DIR}/../app/comm"
    "${CMAKE_CURRENT_LIST_DIR}/../app/utils"
)
target_link_libraries(test_pose_history PRIVATE m pthread)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_pose_history PRIVATE -UNDEBUG)
endif()
add_test(NAME test_pose_history COMMAND test_pose_history)
set_tests_properties(test_pose_history PROPERTIES TIMEOUT 30)

add_executable(test_ocr_postproc
    unit/test_ocr_postproc.c
    "${CMAKE_CURRENT_LIST_DIR}/../app/ai/ocr_postproc.c"
)
target_include_directories(test_ocr_postproc PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../app/ai"
)
target_link_libraries(test_ocr_postproc PRIVATE m)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_ocr_postproc PRIVATE -UNDEBUG)
endif()
add_test(NAME test_ocr_postproc COMMAND test_ocr_postproc)
set_tests_properties(test_ocr_postproc PROPERTIES TIMEOUT 30)

get_filename_component(OCR_PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
file(TO_CMAKE_PATH "${OCR_PROJECT_ROOT}/config/ocr_translator.json"
     OCR_TEST_CONFIG_PATH)
target_compile_definitions(test_config PRIVATE
    OCR_TEST_CONFIG_PATH="${OCR_TEST_CONFIG_PATH}"
)
