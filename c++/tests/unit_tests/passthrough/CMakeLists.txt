# Copyright (c) 2026 Edward Boggis-Rolfe All rights reserved.

# CMakeLists.txt for passthrough unit tests
cmake_minimum_required(VERSION 3.24)

# Include necessary directories
include_directories(${CANOPY_CPP_SOURCE_DIR}/rpc/include)

# Define the test executable for rpc::pass_through
add_executable(passthrough_test passthrough_test.cpp)

target_link_libraries(
  passthrough_test
  GTest::gtest_main
  yas_common
  rpc::rpc
  rpc::mock_test_transport
  common_tests
  test_fixtures
  transport_mock_test_test)

# Add include directories for RPC headers
target_include_directories(
  passthrough_test
  PRIVATE ${CANOPY_CPP_SOURCE_DIR}/rpc/include
          ${CANOPY_CPP_SOURCE_DIR}/telemetry/include
          ${CANOPY_CPP_SOURCE_DIR}/transports/mock_test/include
          ${CANOPY_CPP_SOURCE_DIR}/transports/local/include
          ${CANOPY_CPP_SOURCE_DIR}/tests/common/include
          ${CMAKE_BINARY_DIR}/generated/include
          ${CMAKE_BINARY_DIR}/generated/src)

# Add compile definitions including CANOPY_DEFINES
target_compile_definitions(passthrough_test PRIVATE ${CANOPY_DEFINES})

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(passthrough_test PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()

# Add the tests to CTest
add_test(NAME passthrough_test COMMAND passthrough_test)

include(CTest)
gtest_discover_tests(passthrough_test)
