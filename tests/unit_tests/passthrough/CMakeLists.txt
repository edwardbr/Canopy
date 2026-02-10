# Copyright (c) 2026 Edward Boggis-Rolfe All rights reserved.

# CMakeLists.txt for passthrough unit tests
cmake_minimum_required(VERSION 3.24)

# Include necessary directories
include_directories(${PROJECT_SOURCE_DIR}/rpc/include)

# Define the test executable for rpc::pass_through
add_executable(passthrough_test passthrough_test.cpp)

target_link_libraries(
  passthrough_test
  GTest::gtest_main
  yas_common
  rpc::rpc
  rpc::mock_test_transport
  common_tests
  transport_mock_test_test)

# Add include directories for RPC headers
target_include_directories(
  passthrough_test
  PRIVATE ${PROJECT_SOURCE_DIR}/rpc/include
          ${PROJECT_SOURCE_DIR}/telemetry/include
          ${PROJECT_SOURCE_DIR}/transports/mock_test/include
          ${PROJECT_SOURCE_DIR}/transports/local/include
          ${PROJECT_SOURCE_DIR}/tests/common/include
          ${CMAKE_BINARY_DIR}/generated/include
          ${CMAKE_BINARY_DIR}/generated/src)

# Add compile definitions including CANOPY_DEFINES
target_compile_definitions(passthrough_test PRIVATE ${CANOPY_DEFINES})

# Set C++ standard
set_property(TARGET passthrough_test PROPERTY CXX_STANDARD 17)
set_property(TARGET passthrough_test PROPERTY CXX_STANDARD_REQUIRED ON)

# Add the tests to CTest
add_test(NAME passthrough_test COMMAND passthrough_test)

include(CTest)
gtest_discover_tests(passthrough_test)
