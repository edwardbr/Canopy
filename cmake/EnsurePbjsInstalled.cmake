if(EXISTS "${PBJS_EXECUTABLE}")
  file(TOUCH "${STAMP_FILE}")
  return()
endif()

execute_process(
  COMMAND "${NPM_EXECUTABLE}" install
  WORKING_DIRECTORY "${CLIENT_DIR}"
  RESULT_VARIABLE npm_result)

if(NOT npm_result EQUAL 0)
  message(FATAL_ERROR "Failed to install protobufjs-cli")
endif()

file(TOUCH "${STAMP_FILE}")
