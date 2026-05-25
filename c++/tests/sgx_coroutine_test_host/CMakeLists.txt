#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]
cmake_minimum_required(VERSION 3.24)

if(CANOPY_BUILD_ENCLAVE AND CANOPY_BUILD_COROUTINE)
  add_executable(sgx_coroutine_test_host sgx_coroutine_test_host.cpp)

  target_compile_definitions(sgx_coroutine_test_host PRIVATE ${CANOPY_DEFINES})
  target_include_directories(
    sgx_coroutine_test_host
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated/include>"
           "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated/src>"
    PRIVATE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>" ${CANOPY_INCLUDES})

  target_link_libraries(
    sgx_coroutine_test_host
    PUBLIC example_idl
           io_uring_test_idl
           -Wl,--whole-archive
           test_fixtures
           -Wl,--no-whole-archive
           yas_common
           rpc::rpc
           transport_local
           transport_local_test
           transport_sgx_coroutine
           transport_sgx_coroutine_test
           canopy_io_uring
           GTest::gtest_main
           fmt::fmt-header-only
           spdlog::spdlog
           ${CANOPY_FMT_LIB}
           ${CANOPY_LIBRARIES}
           ${CANOPY_SGX_HOST_LIBRARIES}
           rpc_log)

  target_compile_options(sgx_coroutine_test_host PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
  target_link_options(sgx_coroutine_test_host PRIVATE ${CANOPY_LINK_EXE_OPTIONS} ${CANOPY_SGX_HOST_LINK_OPTIONS})
  target_link_directories(sgx_coroutine_test_host PUBLIC ${CANOPY_SGX_HOST_LIBRARY_DIRS})
  set_property(TARGET sgx_coroutine_test_host PROPERTY COMPILE_PDB_NAME sgx_coroutine_test_host)

  if(CANOPY_ENABLE_CLANG_TIDY)
    set_target_properties(sgx_coroutine_test_host PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
  endif()

  add_dependencies(sgx_coroutine_test_host sgx_coroutine_test_enclave)

  include(CTest)
  if(NOT CANOPY_DEBUG_ADDRESS)
    gtest_discover_tests(sgx_coroutine_test_host)
  endif()
endif()
