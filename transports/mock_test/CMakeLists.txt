# Copyright (c) 2026 Edward Boggis-Rolfe All rights reserved.

# CMakeLists.txt for mock_test transport (testing only)
cmake_minimum_required(VERSION 3.24)

# Create mock_test transport library (header-only + implementation)
add_library(mock_test_transport src/transport.cpp)

target_include_directories(mock_test_transport PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
                                                      ${PROJECT_SOURCE_DIR}/rpc/include)

target_link_libraries(mock_test_transport PUBLIC rpc::rpc)

# Add compile definitions including CANOPY_DEFINES
target_compile_definitions(mock_test_transport PRIVATE ${CANOPY_DEFINES})

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(mock_test_transport PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()

# Add alias for consistent naming
add_library(rpc::mock_test_transport ALIAS mock_test_transport)

# Test interface library for test setup headers
add_library(transport_mock_test_test INTERFACE)
target_include_directories(transport_mock_test_test INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/tests>")
