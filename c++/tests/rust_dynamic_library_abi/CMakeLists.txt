#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

if(CANOPY_BUILD_COROUTINE)
  return()
endif()

set(CANOPY_RUST_TEST_CHILD_TARGET_DIR "${CMAKE_BINARY_DIR}/rust-target")
set(CANOPY_RUST_TEST_CHILD_LIB
    "${CANOPY_RUST_TEST_CHILD_TARGET_DIR}/debug/${CMAKE_SHARED_LIBRARY_PREFIX}canopy_dynamic_library_test_child${CMAKE_SHARED_LIBRARY_SUFFIX}")

CanopyGenerate(
  basic_rpc_probe
  protobuf_runtime_probe/basic_rpc_probe.idl
  "${CMAKE_SOURCE_DIR}/rust/tests"
  "${CMAKE_BINARY_DIR}/generated"
  ""
  yas_binary
  yas_compressed_binary
  yas_json
  protocol_buffers
  include_paths "${CMAKE_SOURCE_DIR}/rust/tests/protobuf_runtime_probe")

add_custom_command(
  OUTPUT "${CANOPY_RUST_TEST_CHILD_LIB}"
  COMMAND
    ${CMAKE_COMMAND} -E env
    CANOPY_GENERATOR=$<TARGET_FILE:generator>
    CANOPY_PROTOC=$<TARGET_FILE:protoc>
    cargo build --package canopy-transport-dynamic-library-test-child --target-dir "${CANOPY_RUST_TEST_CHILD_TARGET_DIR}"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/rust"
  DEPENDS
    generator
    protoc
    "${CMAKE_SOURCE_DIR}/rust/Cargo.toml"
    "${CMAKE_SOURCE_DIR}/rust/transports/dynamic_library/Cargo.toml"
    "${CMAKE_SOURCE_DIR}/rust/transports/dynamic_library/src/lib.rs"
    "${CMAKE_SOURCE_DIR}/rust/transports/dynamic_library/src/ffi.rs"
    "${CMAKE_SOURCE_DIR}/rust/transports/dynamic_library/src/adapter.rs"
    "${CMAKE_SOURCE_DIR}/rust/transports/dynamic_library/src/context.rs"
    "${CMAKE_SOURCE_DIR}/rust/transports/dynamic_library/src/entrypoints.rs"
    "${CMAKE_SOURCE_DIR}/rust/transports/dynamic_library_test_child/Cargo.toml"
    "${CMAKE_SOURCE_DIR}/rust/transports/dynamic_library_test_child/src/lib.rs"
    "${CMAKE_SOURCE_DIR}/rust/transports/dynamic_library_test_child/src/exports.rs"
    "${CMAKE_SOURCE_DIR}/rust/tests/protobuf_runtime_probe/Cargo.toml"
    "${CMAKE_SOURCE_DIR}/rust/tests/protobuf_runtime_probe/build.rs"
    "${CMAKE_SOURCE_DIR}/rust/tests/protobuf_runtime_probe/basic_rpc_probe.idl"
    "${CMAKE_SOURCE_DIR}/rust/tests/protobuf_runtime_probe/src/lib.rs"
  VERBATIM)

add_custom_target(canopy_rust_dynamic_library_test_child ALL DEPENDS "${CANOPY_RUST_TEST_CHILD_LIB}")

add_executable(rust_dynamic_library_abi_test rust_dynamic_library_abi_test.cpp)

target_compile_definitions(
  rust_dynamic_library_abi_test
  PRIVATE ${CANOPY_DEFINES} CANOPY_RUST_TEST_DLL_PATH="${CANOPY_RUST_TEST_CHILD_LIB}")

target_include_directories(
  rust_dynamic_library_abi_test
  PRIVATE "${CMAKE_SOURCE_DIR}"
          "${CMAKE_SOURCE_DIR}/c++/tests/fixtures/include"
          "${CMAKE_SOURCE_DIR}/c++/submodules/yas/include"
          "${CMAKE_SOURCE_DIR}/c++/telemetry/include"
          "${CMAKE_BINARY_DIR}/generated"
          "${CMAKE_BINARY_DIR}/generated/include"
          ${CANOPY_INCLUDES})

target_link_libraries(
  rust_dynamic_library_abi_test
  PRIVATE basic_rpc_probe_idl
          transport_c_abi
          rpc_log
          GTest::gtest_main
          GTest::gmock_main
          fmt::fmt-header-only
          ${CANOPY_FMT_LIB}
          ${CANOPY_LIBRARIES})

target_compile_options(rust_dynamic_library_abi_test PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(rust_dynamic_library_abi_test PRIVATE ${CANOPY_LINK_EXE_OPTIONS})

add_dependencies(rust_dynamic_library_abi_test canopy_rust_dynamic_library_test_child)

include(CTest)
gtest_discover_tests(rust_dynamic_library_abi_test)
