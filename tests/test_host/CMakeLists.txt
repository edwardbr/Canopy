#[[
   Copyright (c) 2024 Edward Boggis-Rolfe
   All rights reserved.
]]
cmake_minimum_required(VERSION 3.24)

if(CANOPY_BUILD_ENCLAVE)
  set(ENCLAVE_DEPENDANCIES marshal_test_edl rpc_telemetry_edl_header)
  set(SGX_TRANSPORT_TEST_LIB transport_sgx_test)
endif()

if(CANOPY_BUILD_COROUTINE)
  set(STREAMING_TRANSPORT_LIB transport_streaming)
  set(STREAMING_TRANSPORT_TEST_LIB transport_streaming_test)
  set(LIBCORO_DLL_TRANSPORT_SOURCES type_test_libcoro_dll_suite.cpp)
  set(LIBCORO_DLL_TRANSPORT_LIB transport_libcoro_dynamic_library)
  set(LIBCORO_DLL_TRANSPORT_TEST_LIB transport_libcoro_dynamic_library_test)
  set(LIBCORO_DLL_TEST_TARGET canopy_libcoro_example_dll)
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
endif()

if(DLL_TEST_TARGET)
  list(APPEND RPC_TEST_COMPILE_DEFINITIONS CANOPY_TEST_DLL_PATH="$<TARGET_FILE:${DLL_TEST_TARGET}>")
endif()

if(LIBCORO_DLL_TEST_TARGET)
  list(APPEND RPC_TEST_COMPILE_DEFINITIONS CANOPY_TEST_LIBCORO_DLL_PATH="$<TARGET_FILE:${LIBCORO_DLL_TEST_TARGET}>")
endif()

if(LIBCORO_IPC_DLL_TEST_TARGET)
  list(APPEND RPC_TEST_COMPILE_DEFINITIONS
       CANOPY_TEST_LIBCORO_SPSC_DLL_PATH="$<TARGET_FILE:${LIBCORO_IPC_DLL_TEST_TARGET}>")
endif()

if(LIBCORO_IPC_DLL_LOADER_TARGET)
  list(APPEND RPC_TEST_COMPILE_DEFINITIONS
       CANOPY_TEST_IPC_CHILD_HOST_PROCESS_PATH="$<TARGET_FILE:${LIBCORO_IPC_DLL_LOADER_TARGET}>")
endif()

if(CANOPY_BUILD_COROUTINE)
  # list(APPEND RPC_TEST_COMPILE_DEFINITIONS
  # CANOPY_TEST_IPC_CHILD_PROCESS_PATH="$<TARGET_FILE:canopy_ipc_child_process>")
endif()

add_executable(
  rpc_test
  main.cpp
  type_test_local_suite.cpp
  remote_type_test_suite.cpp
  hierachical_transport_tests.cpp
  post_functionality_test_suite.cpp
  optimistic_ptr_tests.cpp
  streaming_transport_regression_suite.cpp
  ${DLL_TRANSPORT_SOURCES}
  ${LIBCORO_DLL_TRANSPORT_SOURCES}
  ${LIBCORO_IPC_DLL_TRANSPORT_SOURCES}
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
         transport_direct_test
         transport_local
         transport_local_test
         ${DLL_TRANSPORT_LIB}
         ${DLL_TRANSPORT_TEST_LIB}
         ${LIBCORO_DLL_TRANSPORT_LIB}
         ${LIBCORO_DLL_TRANSPORT_TEST_LIB}
         ${LIBCORO_IPC_DLL_TRANSPORT_LIB}
         ${LIBCORO_IPC_DLL_TRANSPORT_TEST_LIB}
         ${IPC_TRANSPORT_TEST_LIB}
         ${SGX_TRANSPORT_TEST_LIB}
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
         ${CANOPY_LIBRARIES})

if(CANOPY_BUILD_COROUTINE)
  target_link_libraries(
    rpc_test
    PUBLIC transport_ipc_transport
           streaming
           streaming_tcp
           streaming_spsc_queue
           $<$<STREQUAL:${CMAKE_SYSTEM_NAME},Linux>:streaming_io_uring>)
endif()

target_compile_options(rpc_test PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(rpc_test PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
target_link_directories(rpc_test PUBLIC ${SGX_LIBRARY_PATH})
set_property(TARGET rpc_test PROPERTY COMPILE_PDB_NAME rpc_test)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(rpc_test PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()

if(NOT CANOPY_BUILD_COROUTINE)
  add_dependencies(rpc_test ${DLL_TEST_TARGET})
endif()

if(CANOPY_BUILD_COROUTINE)
  add_dependencies(rpc_test ${LIBCORO_DLL_TEST_TARGET})
  add_dependencies(rpc_test ${LIBCORO_IPC_DLL_TEST_TARGET} ${LIBCORO_IPC_DLL_LOADER_TARGET})
endif()

include(CTest)
# Skip gtest_discover_tests when using AddressSanitizer due to protobuf container - overflow false positives
if(NOT CANOPY_DEBUG_ADDRESS)
  gtest_discover_tests(rpc_test)
endif()
