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

if("${CMAKE_CURRENT_SOURCE_DIR}" PATH_EQUAL "${CMAKE_SOURCE_DIR}")
  set(CANOPY_STANDALONE ON)
else()
  set(CANOPY_STANDALONE OFF)
endif()

if(CANOPY_STANDALONE)
  message("RPC standalone")
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
  set(CMAKE_PDB_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
  set(CMAKE_COMPILE_PDB_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)

  set(CMAKE_CXX_STANDARD 17)
  if(CANOPY_BUILD_COROUTINE)
    set(CMAKE_CXX_STANDARD 20)
  endif()
  set(CMAKE_CXX_STANDARD_REQUIRED ON)

  project(rpc_tests VERSION 2.2.0)

  list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")
  include(Canopy)

  add_subdirectory(submodules)
endif()

# Removed: CANOPY_USE_TELEMETRY - use CANOPY_USE_TELEMETRY instead
if(CANOPY_USE_TELEMETRY)
  set(CANOPY_ENCLAVE_TELEMETRY rpc::rpc_telemetry_enclave)
  set(CANOPY_TELEMETRY_ENCLAVE_EDL rpc_telemetry_edl_header_enclave)
  set(CANOPY_TELEMETRY rpc::rpc_telemetry)
  set(CANOPY_TELEMETRY_INTERFACE rpc::rpc_telemetry_interface)
else()
  set(CANOPY_ENCLAVE_TELEMETRY)
  set(CANOPY_TELEMETRY_ENCLAVE_EDL)
  set(CANOPY_TELEMETRY)
  set(CANOPY_TELEMETRY_INTERFACE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")
include(CanopyGenerate)

add_subdirectory(generator)
add_subdirectory(telemetry)
add_subdirectory(rpc)
add_subdirectory(transports)
add_subdirectory(types)

if(CANOPY_BUILD_TEST)
  add_subdirectory(tests)
endif()

if(CANOPY_BUILD_DEMOS)
  add_subdirectory(demos)
endif()

install(FILES "${CMAKE_CURRENT_LIST_DIR}/cmake/FindCanopyGenerate.cmake" DESTINATION ${CMAKE_INSTALL_PREFIX}/cmake/rpc)
