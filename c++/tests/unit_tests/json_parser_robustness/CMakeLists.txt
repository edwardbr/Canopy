# Copyright (c) 2026 Edward Boggis-Rolfe All rights reserved.

cmake_minimum_required(VERSION 3.24)

add_executable(json_parser_robustness_test json_parser_robustness_test.cpp)

target_compile_definitions(json_parser_robustness_test PRIVATE ${CANOPY_DEFINES})

target_link_libraries(json_parser_robustness_test PRIVATE GTest::gtest_main json rpc_log)

target_compile_options(json_parser_robustness_test PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(json_parser_robustness_test PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
set_property(TARGET json_parser_robustness_test PROPERTY COMPILE_PDB_NAME json_parser_robustness_test)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(json_parser_robustness_test PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()

if(CANOPY_BUILD_TEST)
  include(GoogleTest)
  if(NOT CANOPY_DEBUG_ADDRESS)
    gtest_discover_tests(json_parser_robustness_test)
  endif()
endif()
