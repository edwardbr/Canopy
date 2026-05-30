#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]
cmake_minimum_required(VERSION 3.24)

if(CANOPY_BUILD_ENCLAVE AND NOT CANOPY_BUILD_COROUTINE)
  set(ENCLAVE_DEPENDANCIES sgx_blocking_transport_edl rpc_telemetry_edl_header)
  set(SGX_TRANSPORT_TEST_LIB transport_sgx_blocking_test)
endif()

if(CANOPY_BUILD_ENCLAVE AND CANOPY_BUILD_COROUTINE)
  set(SGX_COROUTINE_TRANSPORT_LIB transport_sgx_coroutine)
  set(SGX_COROUTINE_TRANSPORT_TEST_LIB transport_sgx_coroutine_test)
  set(SGX_COROUTINE_ENCLAVE_SERVICE_HOST_TEST_LIB transport_sgx_coroutine_enclave_service_host_test)
  set(SGX_COROUTINE_TRANSPORT_SOURCES sgx_coroutine_transport_test.cpp)
endif()

if(CANOPY_BUILD_COROUTINE)
  set(STREAMING_TRANSPORT_LIB transport_streaming)
  set(STREAMING_TRANSPORT_TEST_LIB connection_factory_test)
  if(CANOPY_SECURE_STREAM_BACKEND STREQUAL "MBEDTLS")
    set(STREAMING_SECURE_STREAM_LIB streaming_mbedtls)
  else()
    set(STREAMING_SECURE_STREAM_LIB streaming_tls)
  endif()
  set(LIBCORO_DLL_SCHEDULED_TRANSPORT_SOURCES type_test_libcoro_dll_scheduled_dll_suite.cpp)
  set(LIBCORO_DLL_SCHEDULED_TRANSPORT_LIB transport_libcoro_dll_scheduled_dynamic_library)
  set(LIBCORO_DLL_SCHEDULED_TRANSPORT_TEST_LIB transport_libcoro_dll_scheduled_dynamic_library_test)
  set(LIBCORO_DLL_SCHEDULED_TEST_TARGET canopy_libcoro_dll_scheduled_example_dll)
  set(LIBCORO_HOST_SCHEDULED_TRANSPORT_SOURCES type_test_libcoro_host_scheduled_dll_suite.cpp)
  set(LIBCORO_HOST_SCHEDULED_TRANSPORT_LIB transport_libcoro_host_scheduled_dynamic_library)
  set(LIBCORO_HOST_SCHEDULED_TRANSPORT_TEST_LIB transport_libcoro_host_scheduled_dynamic_library_test)
  set(LIBCORO_HOST_SCHEDULED_TEST_TARGET canopy_libcoro_host_scheduled_example_dll)
  set(LIBCORO_IPC_DLL_TRANSPORT_SOURCES type_test_libcoro_ipc_dll_suite.cpp)
  set(LIBCORO_IPC_DLL_TRANSPORT_LIB transport_libcoro_spsc_dll_host)
  set(LIBCORO_IPC_DLL_TRANSPORT_TEST_LIB transport_libcoro_spsc_dll_host_test)
  set(LIBCORO_IPC_DLL_TEST_TARGET canopy_libcoro_spsc_example_dll)
  set(LIBCORO_IPC_DLL_LOADER_TARGET canopy_ipc_child_host_process)
  set(IPC_TRANSPORT_TEST_LIB transport_ipc_transport_test)
endif()

if(NOT CANOPY_BUILD_COROUTINE)
  set(DLL_TRANSPORT_SOURCES type_test_dll_suite.cpp)
  set(DLL_TRANSPORT_LIB transport_dynamic_library)
  set(DLL_TRANSPORT_TEST_LIB transport_dynamic_library_test)
  set(DLL_TEST_TARGET canopy_example_dll)
  set(C_ABI_DLL_TRANSPORT_SOURCES type_test_c_abi_dll_suite.cpp)
  set(C_ABI_DLL_TRANSPORT_LIB transport_c_abi)
  set(C_ABI_DLL_TRANSPORT_TEST_LIB transport_c_abi_test)
  set(C_ABI_DLL_TEST_TARGET canopy_example_c_abi_dll)
endif()

if(DLL_TEST_TARGET)
  list(APPEND RPC_TEST_COMPILE_DEFINITIONS CANOPY_TEST_DLL_PATH="$<TARGET_FILE:${DLL_TEST_TARGET}>")
endif()

if(C_ABI_DLL_TEST_TARGET)
  list(APPEND RPC_TEST_COMPILE_DEFINITIONS CANOPY_TEST_C_ABI_DLL_PATH="$<TARGET_FILE:${C_ABI_DLL_TEST_TARGET}>")
endif()

if(LIBCORO_DLL_SCHEDULED_TEST_TARGET)
  list(APPEND RPC_TEST_COMPILE_DEFINITIONS
       CANOPY_TEST_LIBCORO_DLL_SCHEDULED_DLL_PATH="$<TARGET_FILE:${LIBCORO_DLL_SCHEDULED_TEST_TARGET}>")
endif()

if(LIBCORO_HOST_SCHEDULED_TEST_TARGET)
  list(APPEND RPC_TEST_COMPILE_DEFINITIONS
       CANOPY_TEST_LIBCORO_HOST_SCHEDULED_DLL_PATH="$<TARGET_FILE:${LIBCORO_HOST_SCHEDULED_TEST_TARGET}>")
endif()

if(LIBCORO_IPC_DLL_TEST_TARGET)
  list(APPEND RPC_TEST_COMPILE_DEFINITIONS
       CANOPY_TEST_LIBCORO_SPSC_DLL_PATH="$<TARGET_FILE:${LIBCORO_IPC_DLL_TEST_TARGET}>")
endif()

if(LIBCORO_IPC_DLL_LOADER_TARGET)
  list(APPEND RPC_TEST_COMPILE_DEFINITIONS
       CANOPY_TEST_IPC_CHILD_HOST_PROCESS_PATH="$<TARGET_FILE:${LIBCORO_IPC_DLL_LOADER_TARGET}>")
endif()

if(TARGET canopy_sgx_coroutine_sidecar)
  list(APPEND RPC_TEST_COMPILE_DEFINITIONS
       CANOPY_TEST_SGX_COROUTINE_SIDECAR_PATH="$<TARGET_FILE:canopy_sgx_coroutine_sidecar>")
endif()

if(CANOPY_BUILD_ENCLAVE AND SGX_HW)
  list(APPEND RPC_TEST_COMPILE_DEFINITIONS CANOPY_TEST_SGX_HARDWARE_RUNTIME)
endif()

if(CANOPY_BUILD_COROUTINE)
#list(APPEND RPC_TEST_COMPILE_DEFINITIONS CANOPY_TEST_IPC_CHILD_PROCESS_PATH =
#"$<TARGET_FILE:canopy_ipc_child_process>")
  list(APPEND COROUTINE_TESTS attested_streaming_transport_poc_suite.cpp)
endif()

add_executable(
  rpc_test
  main.cpp
  type_test_local_suite.cpp
  remote_type_test_suite.cpp
  hierachical_transport_tests.cpp
  post_functionality_test_suite.cpp
  atomic_smart_ptr_tests.cpp
  optimistic_ptr_tests.cpp
  connection_factory_local_child_test.cpp
  streaming_transport_regression_suite.cpp
  transport_registry_tests.cpp
  ${DLL_TRANSPORT_SOURCES}
  ${C_ABI_DLL_TRANSPORT_SOURCES}
  ${LIBCORO_DLL_SCHEDULED_TRANSPORT_SOURCES}
  ${LIBCORO_HOST_SCHEDULED_TRANSPORT_SOURCES}
  ${LIBCORO_IPC_DLL_TRANSPORT_SOURCES}
  ${SGX_COROUTINE_TRANSPORT_SOURCES}
  ${COROUTINE_TESTS})

target_compile_definitions(rpc_test PRIVATE ${CANOPY_DEFINES} ${RPC_TEST_COMPILE_DEFINITIONS})

target_include_directories(
  rpc_test
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated/include>"
         "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated/src>"
  PRIVATE ${CANOPY_INCLUDES})

target_link_libraries(
  rpc_test
  PUBLIC common_tests
         example_idl
         example_import_idl
         example_shared_idl
         -Wl,--whole-archive
         test_fixtures
         -Wl,--no-whole-archive
         yas_common
         rpc::rpc
         connection_factory
         transport_direct_test
         transport_local
         transport_local_test
         ${DLL_TRANSPORT_LIB}
         ${DLL_TRANSPORT_TEST_LIB}
         ${C_ABI_DLL_TRANSPORT_LIB}
         ${C_ABI_DLL_TRANSPORT_TEST_LIB}
         ${LIBCORO_DLL_SCHEDULED_TRANSPORT_LIB}
         ${LIBCORO_DLL_SCHEDULED_TRANSPORT_TEST_LIB}
         ${LIBCORO_HOST_SCHEDULED_TRANSPORT_LIB}
         ${LIBCORO_HOST_SCHEDULED_TRANSPORT_TEST_LIB}
         ${LIBCORO_IPC_DLL_TRANSPORT_LIB}
         ${LIBCORO_IPC_DLL_TRANSPORT_TEST_LIB}
         ${IPC_TRANSPORT_TEST_LIB}
         ${SGX_TRANSPORT_TEST_LIB}
         ${SGX_COROUTINE_TRANSPORT_LIB}
         ${SGX_COROUTINE_TRANSPORT_TEST_LIB}
         ${SGX_COROUTINE_ENCLAVE_SERVICE_HOST_TEST_LIB}
         ${STREAMING_TRANSPORT_LIB}
         ${STREAMING_TRANSPORT_TEST_LIB}
         yas_common
         args::args
         fmt::fmt-header-only
         GTest::gtest_main
         GTest::gmock_main
         spdlog::spdlog
         ${CANOPY_FMT_LIB}
         ${ENCLAVE_DEPENDANCIES}
         ${CANOPY_LIBRARIES}
         ${CANOPY_SGX_HOST_LIBRARIES}
         rpc_log)

if(CANOPY_BUILD_COROUTINE)
  target_link_libraries(
    rpc_test
    PUBLIC transport_ipc_transport
           route_attestation_protocol_idl
           streaming
           streaming_attestation
           streaming_spsc_queue
           streaming_spsc_wrapping
           canopy::security_attestation
           ${STREAMING_SECURE_STREAM_LIB}
           $<$<STREQUAL:${CMAKE_SYSTEM_NAME},Linux>:streaming_tcp_coroutine>
           $<$<BOOL:${CANOPY_BUILD_WEBSOCKET}>:streaming_websocket>)
  if(CANOPY_BUILD_WEBSOCKET)
    target_compile_definitions(rpc_test PRIVATE CANOPY_BUILD_WEBSOCKET)
  endif()
endif()

target_compile_options(rpc_test PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(rpc_test PRIVATE ${CANOPY_LINK_EXE_OPTIONS} ${CANOPY_SGX_HOST_LINK_OPTIONS})
target_link_directories(rpc_test PUBLIC ${CANOPY_SGX_HOST_LIBRARY_DIRS})
set_property(TARGET rpc_test PROPERTY COMPILE_PDB_NAME rpc_test)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(rpc_test PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()

if(NOT CANOPY_BUILD_COROUTINE)
  add_dependencies(rpc_test ${DLL_TEST_TARGET})
  add_dependencies(rpc_test ${C_ABI_DLL_TEST_TARGET})
  if(CANOPY_BUILD_ENCLAVE)
    add_dependencies(rpc_test marshal_test_enclave)
  endif()
endif()

if(CANOPY_BUILD_COROUTINE)
  add_dependencies(rpc_test ${LIBCORO_DLL_SCHEDULED_TEST_TARGET})
  add_dependencies(rpc_test ${LIBCORO_HOST_SCHEDULED_TEST_TARGET})
  add_dependencies(rpc_test ${LIBCORO_IPC_DLL_TEST_TARGET} ${LIBCORO_IPC_DLL_LOADER_TARGET})
  if(CANOPY_BUILD_ENCLAVE)
    add_dependencies(rpc_test marshal_test_coroutine_enclave)
    if(TARGET canopy_sgx_coroutine_sidecar)
      add_dependencies(rpc_test canopy_sgx_coroutine_sidecar)
    endif()
  endif()
endif()

include(CTest)
#Skip gtest_discover_tests when using AddressSanitizer due to protobuf container - overflow false positives
if(NOT CANOPY_DEBUG_ADDRESS)
  gtest_discover_tests(rpc_test)
endif()

if(CANOPY_BUILD_COROUTINE)
  add_executable(timeout_test timeout_tests.cpp)
  target_compile_definitions(timeout_test PRIVATE ${CANOPY_DEFINES} ${RPC_TEST_COMPILE_DEFINITIONS}
                                                  CANOPY_TIMEOUT_TEST_CERT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/certs")
  target_include_directories(
    timeout_test
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated/include>"
           "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated/src>"
    PRIVATE ${CANOPY_INCLUDES})
  target_link_libraries(
    timeout_test
    PUBLIC common_tests
           example_idl
           -Wl,--whole-archive
           test_fixtures
           -Wl,--no-whole-archive
           yas_common
           rpc::rpc
           transport_local
           transport_local_test
           ${STREAMING_TRANSPORT_LIB}
           ${STREAMING_TRANSPORT_TEST_LIB}
           streaming
           streaming_spsc_queue
           streaming_spsc_wrapping
           ${STREAMING_SECURE_STREAM_LIB}
           $<$<STREQUAL:${CMAKE_SYSTEM_NAME},Linux>:streaming_tcp_coroutine>
           $<$<BOOL:${CANOPY_BUILD_WEBSOCKET}>:streaming_websocket>
           args::args
           fmt::fmt-header-only
           GTest::gtest_main
           GTest::gmock_main
           spdlog::spdlog
           ${CANOPY_FMT_LIB}
           ${CANOPY_LIBRARIES}
           rpc_log)
  if(CANOPY_BUILD_WEBSOCKET)
    target_compile_definitions(timeout_test PRIVATE CANOPY_BUILD_WEBSOCKET)
  endif()
  target_compile_options(timeout_test PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
  target_link_options(timeout_test PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
endif()
