if(NOT DEFINED GNINE_BIN)
  message(FATAL_ERROR "GNINE_BIN is required")
endif()

if(NOT DEFINED REPO_ROOT)
  message(FATAL_ERROR "REPO_ROOT is required")
endif()

if(NOT DEFINED TMP_ROOT)
  message(FATAL_ERROR "TMP_ROOT is required")
endif()

function(run_capture_case scenario example_path scale)
  set(case_root "${TMP_ROOT}/preview-playback-smoke/${scenario}")
  set(frame_prefix "${case_root}/frames/frame.png")
  set(output_image "${case_root}/output.png")
  file(REMOVE_RECURSE "${case_root}")
  file(MAKE_DIRECTORY "${case_root}/frames")

  execute_process(
    COMMAND "${GNINE_BIN}"
            --runtime
            --preview-playback=${scenario}
            --preview-duration-ms=250
            --display-scale=${scale}
            --emit-frames=${frame_prefix}
            "${REPO_ROOT}/${example_path}"
            "${output_image}"
    WORKING_DIRECTORY "${REPO_ROOT}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout_text
    ERROR_VARIABLE stderr_text
  )

  if(NOT result EQUAL 0)
    message(FATAL_ERROR "preview playback capture failed for ${scenario}\nstdout:\n${stdout_text}\nstderr:\n${stderr_text}")
  endif()

  file(GLOB frame_files "${case_root}/frames/frame_*.png")
  list(LENGTH frame_files frame_count)
  if(frame_count LESS 5)
    message(FATAL_ERROR "expected playback capture frames for ${scenario}, found ${frame_count}")
  endif()

  if(NOT EXISTS "${output_image}")
    message(FATAL_ERROR "expected final output image for ${scenario}")
  endif()
endfunction()

run_capture_case("snake" "examples/runtime_snake_v2.psm" 4)
run_capture_case("pong" "examples/runtime_pong_v2.psm" 4)
run_capture_case("flappy" "examples/runtime_flappy_bird_v2.psm" 4)
