add_executable(gnine_semantic_tests semantic_tests.cpp)
target_link_libraries(gnine_semantic_tests PRIVATE gnine_core)

add_test(NAME gnine_semantic_tests COMMAND gnine_semantic_tests)

add_executable(gnine_color_tests color_tests.cpp)
target_link_libraries(gnine_color_tests PRIVATE gnine_core)

add_test(NAME gnine_color_tests COMMAND gnine_color_tests)

add_executable(gnine_runtime_tests runtime_tests.cpp)
target_link_libraries(gnine_runtime_tests PRIVATE gnine_core)

add_test(NAME gnine_runtime_tests COMMAND gnine_runtime_tests)

add_test(NAME gnine_preview_playback_smoke
         COMMAND ${CMAKE_COMMAND}
                 -DGNINE_BIN=$<TARGET_FILE:gnine>
                 -DREPO_ROOT=${GNINE_SOURCE_DIR}
                 -DTMP_ROOT=${CMAKE_BINARY_DIR}
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/preview_playback_smoke.cmake)
