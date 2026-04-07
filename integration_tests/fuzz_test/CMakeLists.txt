#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

# Canopy Shared Pointer Fuzz Test CMakeLists.txt
cmake_minimum_required(VERSION 3.24)

# Create fuzz test for Canopy zone graphs and shared_ptr testing
add_executable(fuzz_test_main fuzz_test_main.cpp)

# Link required libraries for fuzz test
target_link_libraries(
  fuzz_test_main
  PRIVATE rpc::rpc
          transport_local
          fuzz_test_idl # Our independent fuzz test IDL
          test_fixtures
          args::args # Command line argument parsing
          ${LINK_LIBS}
          rpc_log)

# Include directories for fuzz test
target_include_directories(
  fuzz_test_main PRIVATE ${CANOPY_CPP_SOURCE_DIR}/tests/fixtures/include ${CANOPY_CPP_SOURCE_DIR}/tests/common/include
                         ${CMAKE_BINARY_DIR}/generated/include ${CMAKE_BINARY_DIR}/generated/src)

# Compiler options
target_compile_features(fuzz_test_main PRIVATE cxx_std_17)

# Apply the same compile options as other tests
target_compile_definitions(fuzz_test_main PRIVATE ${CANOPY_DEFINES})
target_compile_options(fuzz_test_main PRIVATE ${CANOPY_COMPILE_OPTIONS})
target_link_options(fuzz_test_main PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
target_link_directories(fuzz_test_main PUBLIC ${SGX_LIBRARY_PATH})
set_property(TARGET fuzz_test_main PROPERTY COMPILE_PDB_NAME fuzz_test_main)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(fuzz_test_main PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()

# # Copy test to output directory set_target_properties(fuzz_test_main PROPERTIES RUNTIME_OUTPUT_DIRECTORY
# ${CMAKE_BINARY_DIR}/output/${CMAKE_BUILD_TYPE_LOWER} OUTPUT_NAME fuzz_test_main )

# Enable telemetry for debugging
if(CANOPY_USE_TELEMETRY)
  target_compile_definitions(fuzz_test_main PRIVATE CANOPY_USE_TELEMETRY)
  target_link_libraries(fuzz_test_main PRIVATE rpc_telemetry)
endif()

# Add test to CTest
if(CANOPY_BUILD_TEST)
  add_test(NAME fuzz_test_main COMMAND fuzz_test_main 10) # Run 10 iterations
endif()

# GTest-wrapped version so IDE test discovery (TestMate) can see the fuzz test
include(GoogleTest)
add_executable(fuzz_test_gtest fuzz_test_main.cpp fuzz_gtest_wrapper.cpp)

target_link_libraries(
  fuzz_test_gtest
  PRIVATE rpc::rpc
          transport_local
          fuzz_test_idl
          test_fixtures
          args::args
          ${LINK_LIBS}
          rpc_log
          GTest::gtest_main)

target_include_directories(
  fuzz_test_gtest PRIVATE ${CANOPY_CPP_SOURCE_DIR}/tests/fixtures/include ${CANOPY_CPP_SOURCE_DIR}/tests/common/include
                          ${CMAKE_BINARY_DIR}/generated/include ${CMAKE_BINARY_DIR}/generated/src)

target_compile_features(fuzz_test_gtest PRIVATE cxx_std_17)
target_compile_definitions(fuzz_test_gtest PRIVATE ${CANOPY_DEFINES} "main=fuzz_test_entry")
target_compile_options(fuzz_test_gtest PRIVATE ${CANOPY_COMPILE_OPTIONS})
target_link_options(fuzz_test_gtest PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
target_link_directories(fuzz_test_gtest PUBLIC ${SGX_LIBRARY_PATH})
set_property(TARGET fuzz_test_gtest PROPERTY COMPILE_PDB_NAME fuzz_test_gtest)

if(CANOPY_USE_TELEMETRY)
  target_compile_definitions(fuzz_test_gtest PRIVATE CANOPY_USE_TELEMETRY)
  target_link_libraries(fuzz_test_gtest PRIVATE rpc_telemetry)
endif()

gtest_discover_tests(fuzz_test_gtest)
