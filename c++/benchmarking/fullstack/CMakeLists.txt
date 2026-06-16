#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]
#
# Fullstack Benchmark CMakeLists.txt Benchmarks covering SPSC and TCP transport with multiple encodings
#

cmake_minimum_required(VERSION 3.24)
project(
  benchmarking_fullstack
  VERSION 1.0.0
  LANGUAGES CXX)

include(CanopyCreateBlockingDll)
include(CanopyCreateUnsharedSchedulerDll)
include(CanopyCreateSharedSchedulerDll)
include(CanopyCreateIpcSpscDynamicLibrary)

if(CANOPY_BUILD_COROUTINE)
  canopycreateunsharedschedulerdll(
    canopy_benchmark_unshared_scheduler_dll
    SOURCES
    src/unshared_scheduler_dll_init.cpp
    LINK_LIBRARIES
    comprehensive_idl
    rpc_log)

  canopycreatesharedschedulerdll(
    canopy_benchmark_shared_scheduler_dll
    SOURCES
    src/shared_scheduler_dll_init.cpp
    LINK_LIBRARIES
    comprehensive_idl
    rpc_log)

  canopycreateipcspscdynamiclibrary(
    canopy_benchmark_ipc_spsc_dll
    SOURCES
    src/ipc_spsc_dll_init.cpp
    LINK_LIBRARIES
    comprehensive_idl
    rpc_log)

  add_executable(canopy_benchmark_ipc_child_process src/ipc_child_process_main.cpp)
  target_compile_definitions(canopy_benchmark_ipc_child_process PRIVATE ${CANOPY_DEFINES})
  target_include_directories(
    canopy_benchmark_ipc_child_process PRIVATE "${CANOPY_CPP_SOURCE_DIR}/demos/comprehensive/include"
                                               ${CANOPY_INCLUDES} "${CANOPY_CPP_SOURCE_DIR}/transports/local/include")
  target_link_libraries(
    canopy_benchmark_ipc_child_process
    PRIVATE transport_ipc_spsc
            streaming_spsc_queue
            transport_streaming
            transport_local
            comprehensive_idl
            spsc_queue
            yas_common
            rpc::rpc
            ${CANOPY_LIBRARIES}
            rpc_log)
  target_compile_options(canopy_benchmark_ipc_child_process PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
  target_link_options(canopy_benchmark_ipc_child_process PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
  set_property(TARGET canopy_benchmark_ipc_child_process PROPERTY COMPILE_PDB_NAME canopy_benchmark_ipc_child_process)

else()
  CanopyCreateBlockingDll(
    canopy_benchmark_blocking_dll
    SOURCES
    src/dll_init.cpp
    LINK_LIBRARIES
    comprehensive_idl
    rpc_log)
endif()

set(FULLSTACK_BENCHMARK_SECURE_STREAM_TARGET)
if(CANOPY_SECURE_STREAM_BACKEND STREQUAL "OPENSSL" AND TARGET streaming_openssl_tls)
  set(FULLSTACK_BENCHMARK_SECURE_STREAM_TARGET streaming_openssl_tls)
elseif(
  CANOPY_BUILD_COROUTINE
  AND CANOPY_SECURE_STREAM_BACKEND STREQUAL "MBEDTLS"
  AND TARGET streaming_mbedtls)
  set(FULLSTACK_BENCHMARK_SECURE_STREAM_TARGET streaming_mbedtls)
endif()

add_executable(benchmark src/benchmark.cpp src/benchmark_common.cpp src/benchmark_local.cpp)
if(CANOPY_BUILD_COROUTINE)
  target_sources(
    benchmark
    PRIVATE src/benchmark_unshared_scheduler_dll.cpp
            src/benchmark_shared_scheduler_dll.cpp
            src/benchmark_ipc.cpp
            src/benchmark_spsc.cpp
            src/benchmark_tcp_coroutine.cpp)
endif()
target_compile_definitions(benchmark PRIVATE ${CANOPY_DEFINES})
if(FULLSTACK_BENCHMARK_SECURE_STREAM_TARGET)
  target_compile_definitions(
    benchmark PRIVATE CANOPY_FULLSTACK_BENCHMARK_HAS_TLS
                      CANOPY_FULLSTACK_BENCHMARK_CERT_DIR="${CANOPY_CPP_SOURCE_DIR}/benchmarking/streaming/certs")
endif()
if(CANOPY_BUILD_WEBSOCKET AND TARGET streaming_websocket)
  target_compile_definitions(benchmark PRIVATE CANOPY_BUILD_WEBSOCKET)
endif()
if(CANOPY_BUILD_COROUTINE)
  target_compile_definitions(
    benchmark
    PRIVATE
      CANOPY_BENCHMARK_UNSHARED_SCHEDULER_DLL_PATH="$<TARGET_FILE:canopy_benchmark_unshared_scheduler_dll>"
      CANOPY_BENCHMARK_SHARED_SCHEDULER_DLL_PATH="$<TARGET_FILE:canopy_benchmark_shared_scheduler_dll>"
      CANOPY_BENCHMARK_IPC_SPSC_SIDECAR_PROCESS_PATH="$<TARGET_FILE:canopy_ipc_spsc_sidecar_process>"
      CANOPY_BENCHMARK_IPC_SPSC_DLL_PATH="$<TARGET_FILE:canopy_benchmark_ipc_spsc_dll>"
      CANOPY_BENCHMARK_IPC_CHILD_PROCESS_PATH="$<TARGET_FILE:canopy_benchmark_ipc_child_process>")
else()
  target_sources(benchmark PRIVATE src/benchmark_tcp_blocking.cpp)
  target_compile_definitions(
    benchmark PRIVATE CANOPY_BENCHMARK_BLOCKING_DLL_PATH="$<TARGET_FILE:canopy_benchmark_blocking_dll>")
endif()
target_include_directories(benchmark PRIVATE "${CANOPY_CPP_SOURCE_DIR}/benchmarking/fullstack/src"
                                             "${CANOPY_CPP_SOURCE_DIR}/demos/comprehensive/include" ${CANOPY_INCLUDES})
target_link_libraries(
  benchmark
  PUBLIC comprehensive_idl
         transport_local
         transport_streaming
         connection_factory
         rpc
         ${CANOPY_LIBRARIES})
if(FULLSTACK_BENCHMARK_SECURE_STREAM_TARGET)
  target_link_libraries(benchmark PUBLIC ${FULLSTACK_BENCHMARK_SECURE_STREAM_TARGET})
endif()
if(CANOPY_BUILD_WEBSOCKET AND TARGET streaming_websocket)
  target_link_libraries(benchmark PUBLIC streaming_websocket)
endif()
if(CANOPY_BUILD_COROUTINE)
  target_link_libraries(
    benchmark
    PUBLIC transport_unshared_scheduler_dll
           transport_shared_scheduler_dll
           transport_ipc_spsc
           streaming_spsc_queue
           $<$<STREQUAL:${CMAKE_SYSTEM_NAME},Linux>:streaming_tcp_coroutine>
           rpc_log)
else()
  target_link_libraries(benchmark PUBLIC transport_blocking_dll streaming_tcp_blocking rpc_log)
endif()
target_compile_options(benchmark PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(benchmark PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
set_property(TARGET benchmark PROPERTY COMPILE_PDB_NAME benchmark)

if(CANOPY_BUILD_COROUTINE)
  add_dependencies(
    benchmark
    canopy_benchmark_unshared_scheduler_dll
    canopy_benchmark_shared_scheduler_dll
    canopy_benchmark_ipc_spsc_dll
    canopy_benchmark_ipc_child_process
    canopy_ipc_spsc_sidecar_process)
else()
  add_dependencies(benchmark canopy_benchmark_blocking_dll)
endif()

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(benchmark PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()
