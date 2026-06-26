#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

add_subdirectory(submodules)
add_subdirectory(telemetry)
add_subdirectory(rpc)
add_subdirectory(security/attestation)
add_subdirectory(subcomponents/network_config)
add_subdirectory(subcomponents/rpc_log)
add_subdirectory(subcomponents/spsc_queue)
add_subdirectory(subcomponents/json)
add_subdirectory(subcomponents/modules)
add_subdirectory(subcomponents/io_uring_config)
add_subdirectory(subcomponents/dns_resolver)
add_subdirectory(subcomponents/http_utils)

# The abstract streaming::stream interface is dual-mode; concrete impls inside c++/streaming (tcp_blocking, openssl_tls,
# mbedtls, spsc, attestation, websocket, compression) gate themselves on build options in their own CMakeLists.
add_subdirectory(streaming)
add_subdirectory(subcomponents/http_client)

# io_uring is coroutine-only by design (see project memory). file_system is dual-mode: coroutine builds use the io_uring
# controller, blocking builds use plain POSIX ::open/::read/::write/::close.
if(CANOPY_BUILD_COROUTINE AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_subdirectory(subcomponents/io_uring)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_subdirectory(subcomponents/file_system)
endif()

add_subdirectory(transports)

# The tcp_coroutine stream IDL is generated in all builds so JSON schema users can materialise stream configs
# independently of the coroutine-only implementation. The implementation target still gates itself on coroutine Linux
# builds and on the low-level io_uring controller target.
add_subdirectory(streaming/tcp_coroutine)

# canopy_http_server is dual-mode. OpenSSL TLS can be used in both modes; mbedtls secure streams remain coroutine-only
# in streaming/mbedtls.
if(CANOPY_BUILD_WEBSOCKET)
  add_subdirectory(subcomponents/http_server)
endif()

add_subdirectory(connection_factory)
add_subdirectory(subcomponents/rest)

if(CANOPY_BUILD_TEST)
  add_subdirectory(tests)
endif()

if(CANOPY_BUILD_DEMOS)
  add_subdirectory(demos)
endif()

if(CANOPY_BUILD_BENCHMARKING)
  add_subdirectory(benchmarking)
endif()

if(CANOPY_USE_TELEMETRY)
  set(CANOPY_TELEMETRY rpc::rpc_telemetry)
  set(CANOPY_TELEMETRY_INTERFACE rpc::rpc_telemetry_interface)
else()
  set(CANOPY_TELEMETRY)
  set(CANOPY_TELEMETRY_INTERFACE)
endif()

if(CANOPY_ENABLE_COVERAGE)
  include(CoverageUtilities)
  canopy_add_coverage_targets(rpc_test "*")
endif()
