#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]
# Measures raw stream throughput and round-trip latency. TCP-backed streams are available in blocking and coroutine
# builds; SPSC and io_uring variants are coroutine-only. No transport or service layer is exercised.

cmake_minimum_required(VERSION 3.24)
project(
  benchmarking_streaming
  VERSION 1.0.0
  LANGUAGES CXX)

set(STREAMING_BENCHMARK_SOURCES benchmark.cpp benchmark_common.cpp)
set(STREAMING_BENCHMARK_LIBS
    streaming
    rpc
    fmt::fmt-header-only
    ${CANOPY_LIBRARIES}
    rpc_log)
set(STREAMING_BENCHMARK_DEFINES
    ${CANOPY_DEFINES}
    # Local fixture credentials keep the benchmark independent of demos and avoid using OpenSSL just to generate
    # throwaway certificates when the selected secure-stream backend is mbedTLS.
    CANOPY_STREAMING_BENCHMARK_CERT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/certs")

set(STREAMING_BENCHMARK_SECURE_STREAM_TARGET)
if(CANOPY_SECURE_STREAM_BACKEND STREQUAL "OPENSSL" AND TARGET streaming_openssl_tls)
  set(STREAMING_BENCHMARK_SECURE_STREAM_TARGET streaming_openssl_tls)
elseif(
  CANOPY_BUILD_COROUTINE
  AND CANOPY_SECURE_STREAM_BACKEND STREQUAL "MBEDTLS"
  AND TARGET streaming_mbedtls)
  set(STREAMING_BENCHMARK_SECURE_STREAM_TARGET streaming_mbedtls)
endif()

if(STREAMING_BENCHMARK_SECURE_STREAM_TARGET)
  list(APPEND STREAMING_BENCHMARK_LIBS ${STREAMING_BENCHMARK_SECURE_STREAM_TARGET})
  list(APPEND STREAMING_BENCHMARK_DEFINES CANOPY_STREAMING_BENCHMARK_HAS_TLS)
endif()

if(CANOPY_BUILD_WEBSOCKET AND TARGET streaming_websocket)
  list(APPEND STREAMING_BENCHMARK_LIBS streaming_websocket)
  list(APPEND STREAMING_BENCHMARK_DEFINES CANOPY_BUILD_WEBSOCKET)
endif()

if(CANOPY_BUILD_COROUTINE)
  list(APPEND STREAMING_BENCHMARK_SOURCES stream_spsc.cpp)
  list(APPEND STREAMING_BENCHMARK_LIBS streaming_spsc_queue)

  if(STREAMING_BENCHMARK_SECURE_STREAM_TARGET)
    list(APPEND STREAMING_BENCHMARK_SOURCES stream_tls_spsc.cpp)
  endif()

  if(CANOPY_BUILD_WEBSOCKET AND TARGET streaming_websocket)
    list(APPEND STREAMING_BENCHMARK_SOURCES stream_websocket_spsc.cpp)
    if(STREAMING_BENCHMARK_SECURE_STREAM_TARGET)
      list(APPEND STREAMING_BENCHMARK_SOURCES stream_tls_websocket_spsc.cpp)
    endif()
  endif()

  if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND TARGET streaming_tcp_coroutine)
    list(APPEND STREAMING_BENCHMARK_SOURCES stream_tcp_coroutine.cpp)
    list(APPEND STREAMING_BENCHMARK_LIBS streaming_tcp_coroutine connection_factory)
    list(APPEND STREAMING_BENCHMARK_DEFINES CANOPY_STREAMING_BENCHMARK_HAS_TCP_COROUTINE)
  endif()
else()
  list(APPEND STREAMING_BENCHMARK_SOURCES stream_tcp_blocking.cpp stream_tcp_wrapped.cpp)
  list(APPEND STREAMING_BENCHMARK_LIBS streaming_tcp_blocking)
endif()

add_executable(streaming_benchmark ${STREAMING_BENCHMARK_SOURCES})
target_compile_definitions(streaming_benchmark PRIVATE ${STREAMING_BENCHMARK_DEFINES})
target_include_directories(streaming_benchmark PRIVATE ${CANOPY_INCLUDES})
target_link_libraries(streaming_benchmark PUBLIC ${STREAMING_BENCHMARK_LIBS})

target_compile_options(streaming_benchmark PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(streaming_benchmark PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
set_property(TARGET streaming_benchmark PROPERTY COMPILE_PDB_NAME streaming_benchmark)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(streaming_benchmark PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()
