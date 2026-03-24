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

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(CanopyCreateDynamicLibrary)
include(CanopyCreateLibcoroDynamicLibrary)
include(CanopyCreateLibcoroSpscDynamicLibrary)

if(CANOPY_BUILD_COROUTINE)
  canopycreatelibcorodynamiclibrary(
    canopy_benchmark_libcoro_dll
    SOURCES
    src/canopy_libcoro_dll_init.cpp
    src/rpc_log.cpp
    LINK_LIBRARIES
    comprehensive_idl)

  canopycreatelibcorospscdynamiclibrary(
    canopy_benchmark_libcoro_spsc_dll
    SOURCES
    src/canopy_libcoro_spsc_dll_init.cpp
    src/rpc_log.cpp
    LINK_LIBRARIES
    comprehensive_idl)

  add_executable(canopy_benchmark_ipc_child_process src/ipc_child_process_main.cpp src/rpc_log.cpp)
  target_compile_definitions(canopy_benchmark_ipc_child_process PRIVATE ${CANOPY_DEFINES})
  target_include_directories(
    canopy_benchmark_ipc_child_process PRIVATE "${CMAKE_SOURCE_DIR}/demos/comprehensive/include" ${CANOPY_INCLUDES}
                                               "${CMAKE_SOURCE_DIR}/transports/local/include")
  target_link_libraries(
    canopy_benchmark_ipc_child_process
    PRIVATE transport_ipc_transport
            streaming_spsc_queue
            transport_streaming
            transport_local
            comprehensive_idl
            spsc_queue
            yas_common
            rpc::rpc
            ${CANOPY_LIBRARIES})
  target_compile_options(canopy_benchmark_ipc_child_process PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
  target_link_options(canopy_benchmark_ipc_child_process PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
  set_property(TARGET canopy_benchmark_ipc_child_process PROPERTY COMPILE_PDB_NAME canopy_benchmark_ipc_child_process)
else()
  canopycreatedynamiclibrary(
    canopy_benchmark_dll
    SOURCES
    src/canopy_dll_init.cpp
    src/rpc_log.cpp
    LINK_LIBRARIES
    comprehensive_idl)
endif()

add_executable(benchmark src/benchmark.cpp src/benchmark_common.cpp src/benchmark_local.cpp src/rpc_log.cpp)
if(CANOPY_BUILD_COROUTINE)
  target_sources(
    benchmark
    PRIVATE src/benchmark_libcoro_dynamic_library.cpp
            src/benchmark_ipc.cpp
            src/benchmark_spsc.cpp
            src/benchmark_tcp.cpp
            src/benchmark_io_uring.cpp)
endif()
target_compile_definitions(benchmark PRIVATE ${CANOPY_DEFINES})
if(CANOPY_BUILD_COROUTINE)
  target_compile_definitions(
    benchmark
    PRIVATE CANOPY_BENCHMARK_LIBCORO_DLL_PATH="$<TARGET_FILE:canopy_benchmark_libcoro_dll>"
            CANOPY_BENCHMARK_IPC_CHILD_HOST_PROCESS_PATH="$<TARGET_FILE:canopy_ipc_child_host_process>"
            CANOPY_BENCHMARK_LIBCORO_SPSC_DLL_PATH="$<TARGET_FILE:canopy_benchmark_libcoro_spsc_dll>"
            CANOPY_BENCHMARK_IPC_CHILD_PROCESS_PATH="$<TARGET_FILE:canopy_benchmark_ipc_child_process>")
else()
  target_compile_definitions(benchmark PRIVATE CANOPY_BENCHMARK_DLL_PATH="$<TARGET_FILE:canopy_benchmark_dll>")
endif()
target_include_directories(benchmark PRIVATE "${CMAKE_SOURCE_DIR}/benchmarking/fullstack/src"
                                             "${CMAKE_SOURCE_DIR}/demos/comprehensive/include" ${CANOPY_INCLUDES})
target_link_libraries(benchmark PUBLIC comprehensive_idl transport_local rpc ${CANOPY_LIBRARIES})
if(CANOPY_BUILD_COROUTINE)
  target_link_libraries(
    benchmark
    PUBLIC transport_streaming
           transport_libcoro_dynamic_library
           transport_ipc_transport
           streaming_spsc_queue
           streaming_tcp
           $<$<STREQUAL:${CMAKE_SYSTEM_NAME},Linux>:streaming_io_uring>)
else()
  target_link_libraries(benchmark PUBLIC transport_dynamic_library)
endif()
target_compile_options(benchmark PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(benchmark PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
set_property(TARGET benchmark PROPERTY COMPILE_PDB_NAME benchmark)

if(CANOPY_BUILD_COROUTINE)
  add_dependencies(
    benchmark
    canopy_benchmark_libcoro_dll
    canopy_benchmark_libcoro_spsc_dll
    canopy_benchmark_ipc_child_process
    canopy_ipc_child_host_process)
else()
  add_dependencies(benchmark canopy_benchmark_dll)
endif()

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(benchmark PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()
