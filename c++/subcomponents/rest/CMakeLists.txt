#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

add_library(rest STATIC src/helpers.cpp)

target_include_directories(
  rest
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include>"
  PRIVATE ${CANOPY_INCLUDES})

target_compile_definitions(rest PRIVATE ${CANOPY_DEFINES})
target_compile_options(rest PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(rest PRIVATE ${CANOPY_LINK_OPTIONS})
target_link_libraries(rest PUBLIC json)

add_library(canopy::rest ALIAS rest)

set_property(TARGET rest PROPERTY COMPILE_PDB_NAME rest)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(rest PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()

message(STATUS "rest configured")
