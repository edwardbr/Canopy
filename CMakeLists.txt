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

# Capture Canopy2's own source root so that subdirectory CMakeLists.txt files can reference sibling directories
# correctly even when Canopy2 is consumed via add_subdirectory() (in which case CMAKE_SOURCE_DIR points to the
# consumer).
set(CANOPY_SOURCE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}"
    CACHE INTERNAL "Root of the Canopy2 source tree")

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

include(CanopyGenerate)
include(GNUInstallDirs)

add_subdirectory(generator)
add_subdirectory(telemetry)
add_subdirectory(rpc)
add_subdirectory(transports)
add_subdirectory(types)
add_subdirectory(subcomponents/network_config)
add_subdirectory(subcomponents/rpc_log)
add_subdirectory(subcomponents/spsc_queue)

if(CANOPY_BUILD_COROUTINE)
  add_subdirectory(streaming)
  if(CANOPY_BUILD_WEBSOCKET)
    add_subdirectory(subcomponents/http_server)
  endif()
endif()

if(CANOPY_BUILD_TEST)
  add_subdirectory(tests)
endif()

if(CANOPY_BUILD_DEMOS)
  add_subdirectory(demos)
endif()

if(CANOPY_BUILD_BENCHMARKING)
  add_subdirectory(benchmarking)
endif()

if(CANOPY_ENABLE_COVERAGE)
  include(CoverageUtilities)
  canopy_add_coverage_targets(rpc_test "*")
endif()

# ── CMake modules ───────────────────────────────────────────────────────────
install(
  FILES cmake/CanopyGenerate.cmake
        cmake/CanopyCreateDynamicLibrary.cmake
        cmake/CanopyCreateLibcoroDynamicLibrary.cmake
        cmake/CanopyCreateLibcoroSpscDynamicLibrary.cmake
        cmake/CanopyComponentInstall.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/canopy)

# ── IDL source files ───────────────────────────────────────────────────────── Install non-generated IDL sources so
# downstream projects can regenerate types.
install(FILES rpc/interfaces/rpc/rpc_types.idl DESTINATION ${CMAKE_INSTALL_DATADIR}/canopy/idl/rpc)
install(FILES types/json/interface/json/json.idl DESTINATION ${CMAKE_INSTALL_DATADIR}/canopy/idl/json)

if(CANOPY_BUILD_COROUTINE)
  install(FILES transports/streaming/interface/stream_transport/stream_transport.idl
          DESTINATION ${CMAKE_INSTALL_DATADIR}/canopy/idl/stream_transport)
endif()

# ── Generated headers ───────────────────────────────────────────────────────── The generated/include tree only exists
# after a build; cmake --install runs after the build so by then the directory is present.
install(
  DIRECTORY ${CMAKE_BINARY_DIR}/generated/include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILES_MATCHING
  PATTERN "*.h"
  PATTERN "*.hpp")
