# Copyright (c) 2026 Edward Boggis-Rolfe All rights reserved.

# CMakeLists.txt for member_ptr unit tests
cmake_minimum_required(VERSION 3.24)

# Include necessary directories
include_directories(${PROJECT_SOURCE_DIR}/rpc/include)

# Only build member_ptr_test for non-coroutine builds
# This test is not compatible with coroutine mode
if(NOT CANOPY_BUILD_COROUTINE)
  # idl
  canopygenerate(
    member_ptr_test
    member_ptr_test/member_ptr_test.idl
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_BINARY_DIR}/generated
    ""
    yas_binary
    yas_compressed_binary
    yas_json
    include_paths
    ${CMAKE_CURRENT_SOURCE_DIR}/.
    dependencies
    rpc_types_idl
    install_dir
    ${GENERATED_INSTALL_DIR})

  # Define the test executable for rpc::member_ptr
  add_executable(member_ptr_test member_ptr_test.cpp)

  target_link_libraries(
    member_ptr_test
    GTest::gtest_main
    yas_common
    rpc::rpc
    rpc_types_idl)

  # Add include directories for RPC headers
  target_include_directories(
    member_ptr_test PRIVATE ${PROJECT_SOURCE_DIR}/rpc/include ${PROJECT_SOURCE_DIR}/telemetry/include
                            ${CMAKE_BINARY_DIR}/generated/include ${CMAKE_BINARY_DIR}/generated/src)

  # Set C++ standard
  set_property(TARGET member_ptr_test PROPERTY CXX_STANDARD 17)
  set_property(TARGET member_ptr_test PROPERTY CXX_STANDARD_REQUIRED ON)

  # Add the tests to CTest
  add_test(NAME member_ptr_test COMMAND member_ptr_test)

  include(CTest)
  gtest_discover_tests(member_ptr_test)
endif()
