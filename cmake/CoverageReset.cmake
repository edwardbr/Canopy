#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

if(NOT DEFINED ROOT)
  message(FATAL_ERROR "CoverageReset.cmake requires ROOT to be provided")
endif()

file(GLOB_RECURSE COVERAGE_DATA_FILES "${ROOT}/*.gcda")

foreach(FILE_PATH IN LISTS COVERAGE_DATA_FILES)
  file(REMOVE "${FILE_PATH}")
endforeach()

list(LENGTH COVERAGE_DATA_FILES COVERAGE_DATA_COUNT)
message(STATUS "Coverage reset removed ${COVERAGE_DATA_COUNT} .gcda files from ${ROOT}")
