execute_process(COMMAND "${DEMO}" OUTPUT_FILE "${OUTPUT}"
  RESULT_VARIABLE generated ERROR_VARIABLE error)
if(NOT generated STREQUAL "0")
  message(FATAL_ERROR "IR generation failed: ${error}")
endif()

execute_process(COMMAND "${LLI}" "${OUTPUT}"
  OUTPUT_VARIABLE actual RESULT_VARIABLE executed ERROR_VARIABLE error)
if(NOT executed STREQUAL "0")
  message(FATAL_ERROR "IR execution failed: ${error}")
endif()
if(NOT actual STREQUAL "42\n")
  message(FATAL_ERROR "Expected 42 followed by a newline, got: ${actual}")
endif()
