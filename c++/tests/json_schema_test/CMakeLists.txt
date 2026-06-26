#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

# JSON Schema Generation Tests - consolidated into single test executable

# Single consolidated JSON schema metadata test
add_executable(json_schema_metadata_test json_schema_metadata_test.cpp i_foo_schema_validation_test.cpp
                                         json_convert_test.cpp)

# Add compile definitions like test
target_compile_definitions(json_schema_metadata_test PRIVATE ${CANOPY_DEFINES})
target_compile_definitions(json_schema_metadata_test PRIVATE CANOPY_TEST_BINARY_DIR=\"${CMAKE_BINARY_DIR}\")
target_compile_definitions(json_schema_metadata_test PRIVATE CANOPY_TEST_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\")

# Add include directories to match test structure
target_include_directories(
  json_schema_metadata_test
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated/include>"
         "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated/src>"
  PRIVATE ${CANOPY_INCLUDES})

target_link_libraries(
  json_schema_metadata_test
  PUBLIC example_idl
         example_import_idl
         example_shared_idl
         schema_cycle_idl
         set_fixture_idl
         nullable_fixture_idl
         connection_factory_config_idl
         connection_factory
         network_config_idl
         rpc::rpc
         yas_common
         fmt::fmt-header-only
         json
         GTest::gtest_main
         GTest::gmock_main
         spdlog::spdlog
         rpc_log)

if(TARGET tcp_coroutine_stream_config_idl)
  target_link_libraries(json_schema_metadata_test PUBLIC tcp_coroutine_stream_config_idl)
endif()
if(TARGET tcp_blocking_stream_config_idl)
  target_link_libraries(json_schema_metadata_test PUBLIC tcp_blocking_stream_config_idl)
endif()
if(TARGET compression_stream_config_idl)
  target_link_libraries(json_schema_metadata_test PUBLIC compression_stream_config_idl)
  target_compile_definitions(json_schema_metadata_test PRIVATE CANOPY_CONNECTION_FACTORY_HAS_COMPRESSION)
endif()
if(TARGET http_server_config_idl)
  target_link_libraries(json_schema_metadata_test PUBLIC http_server_config_idl)
  target_compile_definitions(json_schema_metadata_test PRIVATE CANOPY_HAS_HTTP_SERVER_CONFIG)
endif()

# Add compile options and link options like test
target_compile_options(json_schema_metadata_test PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(json_schema_metadata_test PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
set_property(TARGET json_schema_metadata_test PROPERTY COMPILE_PDB_NAME json_schema_metadata_test)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(json_schema_metadata_test PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()

# Add to tests if CTest is enabled - using gtest_discover_tests
if(CANOPY_BUILD_TEST)
  include(GoogleTest)
  # Skip gtest_discover_tests when using AddressSanitizer due to protobuf container-overflow false positives
  if(NOT CANOPY_DEBUG_ADDRESS)
    gtest_discover_tests(json_schema_metadata_test)
  endif()
endif()
