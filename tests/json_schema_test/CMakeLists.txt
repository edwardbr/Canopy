#[[
   Copyright (c) 2024 Edward Boggis-Rolfe
   All rights reserved.
]]

# JSON Schema Generation Tests - consolidated into single test executable

# Single consolidated JSON schema metadata test
add_executable(json_schema_metadata_test json_schema_metadata_test.cpp i_foo_schema_validation_test.cpp
                                         ../fixtures/src/rpc_log.cpp)

# Add compile definitions like test
target_compile_definitions(json_schema_metadata_test PRIVATE ${CANOPY_DEFINES})

# Add include directories to match test structure
target_include_directories(
  json_schema_metadata_test
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated/include>"
         "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated/src>"
  PRIVATE ${CANOPY_INCLUDES})

if(CANOPY_BUILD_ENCLAVE)
  set(ENCLAVE_DEPENDENCIES marshal_test_edl rpc_telemetry_edl_header)
endif()

# Link with libraries similar to test
target_link_libraries(
  json_schema_metadata_test
  PUBLIC common_tests
         example_idl
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
         ${CANOPY_LIBRARIES}
         ${ENCLAVE_DEPENDENCIES}
         test_fixtures)

# Add compile options and link options like test
target_compile_options(json_schema_metadata_test PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(json_schema_metadata_test PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
target_link_directories(json_schema_metadata_test PUBLIC ${SGX_LIBRARY_PATH})
set_property(TARGET json_schema_metadata_test PROPERTY COMPILE_PDB_NAME json_schema_metadata_test)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(json_schema_metadata_test PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()

# Add to tests if CTest is enabled - using gtest_discover_tests
if(CANOPY_BUILD_TEST)
  include(GoogleTest)
  gtest_discover_tests(json_schema_metadata_test)
endif()
