#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]
cmake_minimum_required(VERSION 3.24)

add_executable(serialiser_test serialiser_test.cpp)

target_compile_definitions(serialiser_test PRIVATE ${CANOPY_DEFINES})

target_include_directories(
  serialiser_test
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated/include>"
         "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated/src>"
  PRIVATE ${CANOPY_INCLUDES})

target_link_libraries(
  serialiser_test
  PUBLIC common_tests
         example_idl
         example_import_idl
         example_shared_idl
         yas_common
         rpc::rpc
         transport_local
         args::args
         fmt::fmt-header-only
         GTest::gtest_main
         GTest::gmock_main
         spdlog::spdlog
         ${CANOPY_FMT_LIB}
         ${CANOPY_LIBRARIES})

target_compile_options(serialiser_test PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(serialiser_test PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
target_link_directories(serialiser_test PUBLIC ${SGX_LIBRARY_PATH})
set_property(TARGET serialiser_test PROPERTY COMPILE_PDB_NAME serialiser_test)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(serialiser_test PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()

include(CTest)
gtest_discover_tests(serialiser_test)
