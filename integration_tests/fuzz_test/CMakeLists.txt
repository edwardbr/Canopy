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

if(NOT CANOPY_BUILD_COROUTINE AND CANOPY_BUILD_PROTOCOL_BUFFERS)
  set(CANOPY_RUST_FUZZ_CHILD_TARGET_DIR "${CMAKE_BINARY_DIR}/rust-fuzz-target")
  set(CANOPY_RUST_FUZZ_CHILD_LIB
      "${CANOPY_RUST_FUZZ_CHILD_TARGET_DIR}/debug/${CMAKE_SHARED_LIBRARY_PREFIX}canopy_fuzz_test_rust_child${CMAKE_SHARED_LIBRARY_SUFFIX}"
  )
  set(CANOPY_RUST_FUZZ_CHILD_ENV CANOPY_GENERATOR=$<TARGET_FILE:generator>)
  set(CANOPY_RUST_FUZZ_CHILD_DEPENDS generator)

  if(TARGET protoc)
    list(APPEND CANOPY_RUST_FUZZ_CHILD_ENV CANOPY_PROTOC=$<TARGET_FILE:protoc>)
    list(APPEND CANOPY_RUST_FUZZ_CHILD_DEPENDS protoc)
  endif()

  add_custom_command(
    OUTPUT "${CANOPY_RUST_FUZZ_CHILD_LIB}"
    COMMAND ${CMAKE_COMMAND} -E env ${CANOPY_RUST_FUZZ_CHILD_ENV} cargo build --manifest-path
            "${CMAKE_CURRENT_SOURCE_DIR}/rust_child/Cargo.toml" --target-dir "${CANOPY_RUST_FUZZ_CHILD_TARGET_DIR}"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    DEPENDS ${CANOPY_RUST_FUZZ_CHILD_DEPENDS}
            "${CMAKE_CURRENT_SOURCE_DIR}/rust_child/Cargo.toml"
            "${CMAKE_CURRENT_SOURCE_DIR}/rust_child/src/lib.rs"
            "${CMAKE_SOURCE_DIR}/rust/rpc/Cargo.toml"
            "${CMAKE_SOURCE_DIR}/rust/rpc/src/internal/base.rs"
            "${CMAKE_SOURCE_DIR}/rust/rpc/src/internal/casting_interface.rs"
            "${CMAKE_SOURCE_DIR}/rust/rpc/src/internal/pass_through.rs"
            "${CMAKE_SOURCE_DIR}/rust/rpc/src/internal/service.rs"
            "${CMAKE_SOURCE_DIR}/rust/rpc/src/internal/service_proxy.rs"
            "${CMAKE_SOURCE_DIR}/rust/rpc/src/internal/stub.rs"
            "${CMAKE_SOURCE_DIR}/rust/rpc/src/internal/transport.rs"
            "${CMAKE_SOURCE_DIR}/rust/rpc/src/lib.rs"
            "${CMAKE_SOURCE_DIR}/rust/transports/dynamic_library/Cargo.toml"
            "${CMAKE_SOURCE_DIR}/rust/transports/dynamic_library/src/ffi.rs"
            "${CMAKE_SOURCE_DIR}/rust/transports/dynamic_library/src/lib.rs"
            "${CMAKE_SOURCE_DIR}/rust/transports/dynamic_library/src/service_endpoint.rs"
            "${CMAKE_SOURCE_DIR}/rust/transports/local/Cargo.toml"
            "${CMAKE_SOURCE_DIR}/rust/transports/local/src/lib.rs"
    VERBATIM)

  add_custom_target(canopy_fuzz_test_rust_child DEPENDS "${CANOPY_RUST_FUZZ_CHILD_LIB}")
endif()

target_link_libraries(
  fuzz_test_gtest
  PRIVATE rpc::rpc
          transport_c_abi
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
target_compile_definitions(fuzz_test_gtest PRIVATE ${CANOPY_DEFINES} "main=fuzz_test_entry" CANOPY_FUZZ_TEST_GTEST)
if(NOT CANOPY_BUILD_COROUTINE AND CANOPY_BUILD_PROTOCOL_BUFFERS)
  target_compile_definitions(fuzz_test_gtest PRIVATE CANOPY_RUST_FUZZ_TEST_DLL_PATH="${CANOPY_RUST_FUZZ_CHILD_LIB}")
  add_dependencies(fuzz_test_gtest canopy_fuzz_test_rust_child)
endif()
target_compile_options(fuzz_test_gtest PRIVATE ${CANOPY_COMPILE_OPTIONS})
target_link_options(fuzz_test_gtest PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
target_link_directories(fuzz_test_gtest PUBLIC ${SGX_LIBRARY_PATH})
set_property(TARGET fuzz_test_gtest PROPERTY COMPILE_PDB_NAME fuzz_test_gtest)

if(CANOPY_USE_TELEMETRY)
  target_compile_definitions(fuzz_test_gtest PRIVATE CANOPY_USE_TELEMETRY)
  target_link_libraries(fuzz_test_gtest PRIVATE rpc_telemetry)
endif()

gtest_discover_tests(fuzz_test_gtest)
