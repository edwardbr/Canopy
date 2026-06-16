#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]
cmake_minimum_required(VERSION 3.24)

set(SOURCES ${COROUTINE_SERVICE_PROXIES} src/foo_impl.cpp src/tests.cpp)

add_library(common_tests STATIC ${SOURCES})

target_compile_definitions(common_tests PRIVATE ${CANOPY_DEFINES})
target_include_directories(
  common_tests
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include/marshalled_tests>"
  PRIVATE ${CANOPY_INCLUDES})

target_compile_options(common_tests PRIVATE ${CANOPY_COMPILE_OPTIONS})
target_link_options(common_tests PRIVATE ${CANOPY_LINK_OPTIONS})

set(CORO_IDL)

target_link_libraries(
  common_tests
  PRIVATE rpc::rpc
          ${ADDITIONAL_TARGET}
          example_idl
          example_import_idl
          example_shared_idl
          transport_local
          ${CANOPY_FMT_LIB}
          ${CORO_IDL}
          GTest::gtest_main
          GTest::gmock_main
          yas_common)

set_property(TARGET common_tests PROPERTY COMPILE_PDB_NAME common_tests)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(common_tests PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()
