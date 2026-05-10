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

include(CanopyCreateDynamicLibrary)
include(CanopyCreateLibcoroDllScheduledDynamicLibrary)
include(CanopyCreateLibcoroHostScheduledDynamicLibrary)
include(CanopyCreateLibcoroSpscDynamicLibrary)

if(CANOPY_BUILD_COROUTINE)
  canopycreatelibcorodllscheduleddynamiclibrary(
    canopy_benchmark_libcoro_dll_scheduled_dll
    SOURCES
    src/canopy_libcoro_dll_scheduled_dll_init.cpp
    LINK_LIBRARIES
    comprehensive_idl
    rpc_log)

  canopycreatelibcorohostscheduleddynamiclibrary(
    canopy_benchmark_libcoro_host_scheduled_dll
    SOURCES
    src/canopy_libcoro_host_scheduled_dll_init.cpp
    LINK_LIBRARIES
    comprehensive_idl
    rpc_log)

  canopycreatelibcorospscdynamiclibrary(
    canopy_benchmark_libcoro_spsc_dll
    SOURCES
    src/canopy_libcoro_spsc_dll_init.cpp
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
    PRIVATE transport_ipc_transport
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

  if(CANOPY_BUILD_ENCLAVE)
    set(BENCHMARK_SGX_COROUTINE_TELEMETRY_ENCLAVE_LIBS)
    if(CANOPY_USE_TELEMETRY)
      list(APPEND BENCHMARK_SGX_COROUTINE_TELEMETRY_ENCLAVE_LIBS rpc_telemetry_enclave)
    endif()

    set(BENCHMARK_SGX_COROUTINE_PROTOBUF_ENCLAVE_LIBS)
    if(TARGET canopy_nanopb_runtime_enclave)
      list(APPEND BENCHMARK_SGX_COROUTINE_PROTOBUF_ENCLAVE_LIBS canopy_nanopb_runtime_enclave)
    endif()
    if(TARGET rpc_types_idl_enclave AND (CANOPY_BUILD_PROTOCOL_BUFFERS OR CANOPY_BUILD_NANOPB))
      list(APPEND BENCHMARK_SGX_COROUTINE_PROTOBUF_ENCLAVE_LIBS rpc_types_idl_enclave)
    endif()

    add_enclave_library(
      canopy_benchmark_sgx_coroutine_enclave
      SRCS src/benchmark_sgx_coroutine_enclave.cpp
      HEADER_ONLY_LIBS ${CANOPY_ENCLAVE_FMT_LIB}
      TRUSTED_LIBS
        libcoro_enclave
        rpc_enclave
        streaming_enclave
        transport_streaming_enclave
        streaming_transport_idl_enclave
        transport_sgx_coroutine_enclave
        ${BENCHMARK_SGX_COROUTINE_TELEMETRY_ENCLAVE_LIBS}
        streaming_spsc_queue_enclave
        streaming_io_uring_new_enclave
        canopy_coroutine_edl_enclave
        transport_local_enclave
        canopy_io_uring_enclave
        comprehensive_idl_enclave
        yas_common
        ${BENCHMARK_SGX_COROUTINE_PROTOBUF_ENCLAVE_LIBS}
        ${SGX_ENCLAVE_LIBS})

    target_compile_definitions(canopy_benchmark_sgx_coroutine_enclave PRIVATE ${CANOPY_ENCLAVE_DEFINES})
    target_include_directories(canopy_benchmark_sgx_coroutine_enclave PRIVATE ${CANOPY_ENCLAVE_LIBCXX_INCLUDES})
    target_compile_options(canopy_benchmark_sgx_coroutine_enclave PRIVATE ${CANOPY_ENCLAVE_COMPILE_OPTIONS}
                                                                          ${CANOPY_WARN_OK_ENCLAVE})
    target_link_options(canopy_benchmark_sgx_coroutine_enclave PRIVATE ${CANOPY_ENCLAVE_LINK_OPTIONS})
    set_property(TARGET canopy_benchmark_sgx_coroutine_enclave PROPERTY COMPILE_PDB_NAME
                                                                        canopy_benchmark_sgx_coroutine_enclave)

    enclave_sign(
      canopy_benchmark_sgx_coroutine_enclave
      KEY ../../tests/test_enclave/marshal_test_enclave_private.pem
      CONFIG benchmark_sgx_coroutine_enclave.config.${SGX_MODE}.xml)
  endif()
else()
  canopycreatedynamiclibrary(
    canopy_benchmark_dll
    SOURCES
    src/canopy_dll_init.cpp
    LINK_LIBRARIES
    comprehensive_idl
    rpc_log)
endif()

add_executable(benchmark src/benchmark.cpp src/benchmark_common.cpp src/benchmark_local.cpp)
if(CANOPY_BUILD_COROUTINE)
  target_sources(
    benchmark
    PRIVATE src/benchmark_libcoro_dll_scheduled_dynamic_library.cpp
            src/benchmark_libcoro_host_scheduled_dynamic_library.cpp
            src/benchmark_ipc.cpp
            src/benchmark_spsc.cpp
            src/benchmark_tcp.cpp
            src/benchmark_io_uring.cpp)
  if(CANOPY_BUILD_ENCLAVE)
    target_sources(benchmark PRIVATE src/benchmark_sgx_coroutine.cpp)
  endif()
endif()
target_compile_definitions(benchmark PRIVATE ${CANOPY_DEFINES})
if(CANOPY_BUILD_COROUTINE)
  target_compile_definitions(
    benchmark
    PRIVATE
      CANOPY_BENCHMARK_LIBCORO_DLL_SCHEDULED_DLL_PATH="$<TARGET_FILE:canopy_benchmark_libcoro_dll_scheduled_dll>"
      CANOPY_BENCHMARK_LIBCORO_HOST_SCHEDULED_DLL_PATH="$<TARGET_FILE:canopy_benchmark_libcoro_host_scheduled_dll>"
      CANOPY_BENCHMARK_IPC_CHILD_HOST_PROCESS_PATH="$<TARGET_FILE:canopy_ipc_child_host_process>"
      CANOPY_BENCHMARK_LIBCORO_SPSC_DLL_PATH="$<TARGET_FILE:canopy_benchmark_libcoro_spsc_dll>"
      CANOPY_BENCHMARK_IPC_CHILD_PROCESS_PATH="$<TARGET_FILE:canopy_benchmark_ipc_child_process>")
  if(CANOPY_BUILD_ENCLAVE)
    target_compile_definitions(
      benchmark
      PRIVATE CANOPY_BENCHMARK_SGX_COROUTINE
              CANOPY_BENCHMARK_SGX_COROUTINE_ENCLAVE_PATH="${canopy_benchmark_sgx_coroutine_enclave_sign_OUTPUT_NAME}")
  endif()
else()
  target_compile_definitions(benchmark PRIVATE CANOPY_BENCHMARK_DLL_PATH="$<TARGET_FILE:canopy_benchmark_dll>")
endif()
target_include_directories(benchmark PRIVATE "${CANOPY_CPP_SOURCE_DIR}/benchmarking/fullstack/src"
                                             "${CANOPY_CPP_SOURCE_DIR}/demos/comprehensive/include" ${CANOPY_INCLUDES})
target_link_libraries(benchmark PUBLIC comprehensive_idl transport_local rpc ${CANOPY_LIBRARIES})
if(CANOPY_BUILD_COROUTINE)
  target_link_libraries(
    benchmark
    PUBLIC transport_streaming
           transport_libcoro_dll_scheduled_dynamic_library
           transport_libcoro_host_scheduled_dynamic_library
           transport_ipc_transport
           streaming_spsc_queue
           streaming_tcp
           $<$<STREQUAL:${CMAKE_SYSTEM_NAME},Linux>:streaming_io_uring_new>
           rpc_log)
  if(CANOPY_BUILD_ENCLAVE)
    target_link_libraries(benchmark PUBLIC transport_sgx_coroutine)
  endif()
else()
  target_link_libraries(benchmark PUBLIC transport_dynamic_library rpc_log)
endif()
target_compile_options(benchmark PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(benchmark PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
set_property(TARGET benchmark PROPERTY COMPILE_PDB_NAME benchmark)

if(CANOPY_BUILD_COROUTINE)
  add_dependencies(
    benchmark
    canopy_benchmark_libcoro_dll_scheduled_dll
    canopy_benchmark_libcoro_host_scheduled_dll
    canopy_benchmark_libcoro_spsc_dll
    canopy_benchmark_ipc_child_process
    canopy_ipc_child_host_process)
  if(CANOPY_BUILD_ENCLAVE)
    add_dependencies(benchmark canopy_benchmark_sgx_coroutine_enclave)
  endif()
else()
  add_dependencies(benchmark canopy_benchmark_dll)
endif()

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(benchmark PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()
