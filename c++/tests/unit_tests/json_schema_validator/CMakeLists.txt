# Copyright (c) 2026 Edward Boggis-Rolfe All rights reserved.

cmake_minimum_required(VERSION 3.24)

add_executable(json_schema_validator_test json_schema_validator_test.cpp)

target_compile_definitions(json_schema_validator_test PRIVATE ${CANOPY_DEFINES})

target_link_libraries(json_schema_validator_test PRIVATE GTest::gtest_main json connection_factory rpc_log)

target_compile_options(json_schema_validator_test PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(json_schema_validator_test PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
set_property(TARGET json_schema_validator_test PROPERTY COMPILE_PDB_NAME json_schema_validator_test)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(json_schema_validator_test PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()

if(CANOPY_BUILD_TEST)
  include(GoogleTest)
  if(NOT CANOPY_DEBUG_ADDRESS)
    gtest_discover_tests(json_schema_validator_test)
  endif()
endif()
