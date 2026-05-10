#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

add_subdirectory(submodules)
add_subdirectory(telemetry)
add_subdirectory(rpc)
add_subdirectory(subcomponents/network_config)
add_subdirectory(subcomponents/rpc_log)
add_subdirectory(subcomponents/spsc_queue)

if(CANOPY_BUILD_COROUTINE)
  add_subdirectory(streaming)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_subdirectory(subcomponents/io_uring)
  endif()
  if(CANOPY_BUILD_WEBSOCKET)
    add_subdirectory(subcomponents/http_server)
  endif()
  if(CANOPY_BUILD_ENCLAVE)
    add_subdirectory(transports/sgx_coroutine/edl)
  endif()
endif()

add_subdirectory(transports)

if(CANOPY_BUILD_COROUTINE AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # The new io_uring stream adapter depends on the low-level io_uring controller target and optionally on
  # transport_streaming test helpers, so it is configured after both subcomponents/io_uring and transports.
  add_subdirectory(streaming/io_uring_new)
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

# Removed: CANOPY_USE_TELEMETRY - use CANOPY_USE_TELEMETRY instead
if(CANOPY_USE_TELEMETRY)
  set(CANOPY_ENCLAVE_TELEMETRY)
  set(CANOPY_TELEMETRY_ENCLAVE_EDL rpc_telemetry_edl_header_enclave)
  set(CANOPY_TELEMETRY rpc::rpc_telemetry)
  set(CANOPY_TELEMETRY_INTERFACE rpc::rpc_telemetry_interface)
else()
  set(CANOPY_ENCLAVE_TELEMETRY)
  set(CANOPY_TELEMETRY_ENCLAVE_EDL)
  set(CANOPY_TELEMETRY)
  set(CANOPY_TELEMETRY_INTERFACE)
endif()

if(CANOPY_ENABLE_COVERAGE)
  include(CoverageUtilities)
  canopy_add_coverage_targets(rpc_test "*")
endif()
