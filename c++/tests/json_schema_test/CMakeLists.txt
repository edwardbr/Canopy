#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

# JSON Schema Generation Tests - consolidated into single test executable

# Single consolidated JSON schema metadata test
add_executable(json_schema_metadata_test json_schema_metadata_test.cpp i_foo_schema_validation_test.cpp)

# Add compile definitions like test
target_compile_definitions(json_schema_metadata_test PRIVATE ${CANOPY_DEFINES})

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
         rpc::rpc
         yas_common
         fmt::fmt-header-only
         GTest::gtest_main
         GTest::gmock_main
         spdlog::spdlog
         nlohmann_json::nlohmann_json
         nlohmann_json_schema_validator
         rpc_log)

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
