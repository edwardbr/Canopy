#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]
# Measures raw stream throughput and round-trip latency for each stream type: spsc, tcp, io_uring (Linux), tls+spsc,
# ws+spsc, and tls+ws+spsc. No transport or service layer is exercised.

cmake_minimum_required(VERSION 3.24)
project(
  benchmarking_streaming
  VERSION 1.0.0
  LANGUAGES CXX)

if(CANOPY_SECURE_STREAM_BACKEND STREQUAL "MBEDTLS")
  set(STREAMING_BENCHMARK_SECURE_STREAM_TARGET streaming_mbedtls)
elseif(CANOPY_SECURE_STREAM_BACKEND STREQUAL "OPENSSL")
  set(STREAMING_BENCHMARK_SECURE_STREAM_TARGET streaming_tls)
else()
  message(FATAL_ERROR "Unsupported CANOPY_SECURE_STREAM_BACKEND for streaming_benchmark: ${CANOPY_SECURE_STREAM_BACKEND}")
endif()

if(NOT TARGET ${STREAMING_BENCHMARK_SECURE_STREAM_TARGET})
  message(FATAL_ERROR "streaming_benchmark requires ${STREAMING_BENCHMARK_SECURE_STREAM_TARGET}; check CANOPY_SECURE_STREAM_BACKEND")
endif()

add_executable(
  streaming_benchmark
  benchmark.cpp
  benchmark_common.cpp
  stream_spsc.cpp
  stream_tcp.cpp
  stream_io_uring.cpp
  stream_sgx_io_uring.cpp
  stream_tls_spsc.cpp
  stream_websocket_spsc.cpp
  stream_tls_websocket_spsc.cpp)
target_compile_definitions(
  streaming_benchmark
  PRIVATE ${CANOPY_DEFINES}
          # Local fixture credentials keep the benchmark independent of demos and avoid using OpenSSL just to generate
          # throwaway certificates when the selected secure-stream backend is mbedTLS.
          CANOPY_STREAMING_BENCHMARK_CERT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/certs")
if(CANOPY_BUILD_WEBSOCKET)
  target_compile_definitions(streaming_benchmark PRIVATE CANOPY_BUILD_WEBSOCKET)
endif()
target_include_directories(streaming_benchmark PRIVATE ${CANOPY_INCLUDES})
target_link_libraries(
  streaming_benchmark
  PUBLIC streaming
         streaming_tcp
         ${STREAMING_BENCHMARK_SECURE_STREAM_TARGET}
         streaming_spsc_queue
         rpc
         fmt::fmt-header-only
         ${CANOPY_LIBRARIES}
         rpc_log)

if(CANOPY_BUILD_WEBSOCKET)
  target_link_libraries(streaming_benchmark PUBLIC streaming_websocket wslay)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(streaming_benchmark PUBLIC streaming_io_uring)
endif()

if(CANOPY_BUILD_ENCLAVE
   AND CANOPY_BUILD_COROUTINE
   AND TARGET sgx_coroutine_test_enclave
   AND TARGET test_fixtures)
  target_compile_definitions(streaming_benchmark PRIVATE CANOPY_STREAMING_BENCHMARK_SGX_IO_URING)
  target_include_directories(streaming_benchmark PRIVATE "${CMAKE_SOURCE_DIR}/c++/tests/sgx_coroutine_test_host")
  target_link_libraries(
    streaming_benchmark
    PUBLIC io_uring_test_idl
           example_idl
           test_fixtures
           transport_sgx_coroutine
           transport_sgx_coroutine_test
           canopy_io_uring
           ${CANOPY_SGX_HOST_LIBRARIES})
  target_link_options(streaming_benchmark PRIVATE ${CANOPY_SGX_HOST_LINK_OPTIONS})
  target_link_directories(streaming_benchmark PUBLIC ${SGX_LIBRARY_PATH})
  add_dependencies(streaming_benchmark sgx_coroutine_test_enclave)
endif()
target_compile_options(streaming_benchmark PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(streaming_benchmark PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
set_property(TARGET streaming_benchmark PROPERTY COMPILE_PDB_NAME streaming_benchmark)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(streaming_benchmark PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()
