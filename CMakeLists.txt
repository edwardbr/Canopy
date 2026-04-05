#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

project(
  rpc
  VERSION 2.2.0
  LANGUAGES CXX)

message("configuring RPC")

set(CANOPY_CPP_SOURCE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/c++"
    CACHE INTERNAL "Root of the Canopy C++ source tree")
set(CANOPY_INTERFACE_SOURCE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/interfaces"
    CACHE INTERNAL "Root of the shared Canopy interface tree")

if("${CMAKE_CURRENT_SOURCE_DIR}" PATH_EQUAL "${CMAKE_SOURCE_DIR}")
  message("RPC standalone")
  project(rpc_standalone VERSION 3.0.0)

  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
  set(CMAKE_PDB_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
  set(CMAKE_COMPILE_PDB_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
endif()

# Set up module path and dependencies whether used standalone or as a subdirectory. DEPENDENCIES_LOADED is set by
# Canopy.cmake, so a downstream project that already included cmake/Canopy.cmake before add_subdirectory() will not
# double-initialise.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")
if(NOT DEPENDENCIES_LOADED)
  include(Canopy)
  add_subdirectory(submodules)
endif()

include(CanopyGenerate)

add_subdirectory(generator)
add_subdirectory("${CANOPY_INTERFACE_SOURCE_DIR}" interfaces)
add_subdirectory("${CANOPY_CPP_SOURCE_DIR}" c++)
