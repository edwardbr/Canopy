#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.

   CoverageUtilities.cmake - Coverage target helpers
]]

include_guard(GLOBAL)

function(canopy_add_coverage_targets coverage_test_target coverage_gtest_filter)
  if(NOT CANOPY_ENABLE_COVERAGE OR NOT CANOPY_BUILD_TEST)
    return()
  endif()

  if(NOT coverage_test_target)
    message(FATAL_ERROR "canopy_add_coverage_targets requires a test target name")
  endif()

  if(NOT TARGET "${coverage_test_target}")
    return()
  endif()

  if(NOT coverage_gtest_filter)
    set(coverage_gtest_filter "type_test/*")
  endif()

  set(_coverage_test_id "${coverage_test_target}")
  string(REGEX REPLACE "[^A-Za-z0-9_.+-]" "_" _coverage_test_id "${_coverage_test_id}")
  set(_coverage_run_target "coverage-run-${_coverage_test_id}")

  set(CANOPY_COVERAGE_OUTPUT_DIR
      "${CMAKE_BINARY_DIR}/coverage"
      CACHE PATH "Output directory for coverage reports")
  set(CANOPY_COVERAGE_GTEST_FILTER "${coverage_gtest_filter}")

  find_program(GCOVR_EXECUTABLE gcovr)
  find_program(LCOV_EXECUTABLE lcov)
  find_program(GENHTML_EXECUTABLE genhtml)

  add_custom_target(
    coverage-reset
    COMMAND ${CMAKE_COMMAND} -DROOT=${CMAKE_BINARY_DIR} -P ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/CoverageReset.cmake
    COMMENT "Delete existing .gcda files in the current build tree")

  add_custom_target(
    "${_coverage_run_target}"
    COMMAND
      ${CMAKE_COMMAND} -E chdir "$<TARGET_FILE_DIR:${coverage_test_target}>" "$<TARGET_FILE:${coverage_test_target}>"
      --telemetry-console "--gtest_filter=${CANOPY_COVERAGE_GTEST_FILTER}"
    DEPENDS coverage-reset "${coverage_test_target}"
    USES_TERMINAL
    COMMENT "Run ${coverage_test_target} in single-process mode for coverage data collection")

  if(GCOVR_EXECUTABLE)
    add_custom_target(
      coverage-html-gcovr
      COMMAND ${CMAKE_COMMAND} -E make_directory "${CANOPY_COVERAGE_OUTPUT_DIR}/gcovr"
      COMMAND
        "${GCOVR_EXECUTABLE}" --root "${CMAKE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}" --exclude
        "${CMAKE_SOURCE_DIR}/submodules/.*" --exclude "${CMAKE_BINARY_DIR}/generated/.*" --exclude
        "${CMAKE_BINARY_DIR}/_deps/.*" --html-details "${CANOPY_COVERAGE_OUTPUT_DIR}/gcovr/index.html" --print-summary
        --sort=filename
      DEPENDS "${_coverage_run_target}"
      USES_TERMINAL
      COMMENT "Generate HTML coverage report with gcovr")
  else()
    message(STATUS "gcovr not found: coverage-html-gcovr target will be unavailable")
  endif()

  if(LCOV_EXECUTABLE AND GENHTML_EXECUTABLE)
    add_custom_target(
      coverage-html-lcov
      COMMAND ${CMAKE_COMMAND} -E make_directory "${CANOPY_COVERAGE_OUTPUT_DIR}/lcov/html"
      COMMAND "${LCOV_EXECUTABLE}" --capture --directory "${CMAKE_BINARY_DIR}" --output-file
              "${CANOPY_COVERAGE_OUTPUT_DIR}/lcov/raw.info"
      COMMAND
        "${LCOV_EXECUTABLE}" --remove "${CANOPY_COVERAGE_OUTPUT_DIR}/lcov/raw.info" "/usr/*"
        "${CMAKE_SOURCE_DIR}/submodules/*" "${CMAKE_BINARY_DIR}/generated/*" "${CMAKE_BINARY_DIR}/_deps/*" --output-file
        "${CANOPY_COVERAGE_OUTPUT_DIR}/lcov/filtered.info"
      COMMAND "${GENHTML_EXECUTABLE}" "${CANOPY_COVERAGE_OUTPUT_DIR}/lcov/filtered.info" --output-directory
              "${CANOPY_COVERAGE_OUTPUT_DIR}/lcov/html" --title "Canopy Coverage (lcov)"
      DEPENDS "${_coverage_run_target}"
      USES_TERMINAL
      COMMENT "Generate HTML coverage report with lcov/genhtml")
  else()
    message(STATUS "lcov/genhtml not found: coverage-html-lcov target will be unavailable")
  endif()

  if(GCOVR_EXECUTABLE)
    add_custom_target(coverage-html DEPENDS coverage-html-gcovr)
  elseif(LCOV_EXECUTABLE AND GENHTML_EXECUTABLE)
    add_custom_target(coverage-html DEPENDS coverage-html-lcov)
  endif()
endfunction()
