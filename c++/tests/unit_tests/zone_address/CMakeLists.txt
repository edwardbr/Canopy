# Copyright (c) 2026 Edward Boggis-Rolfe All rights reserved.

cmake_minimum_required(VERSION 3.24)

include_directories(${CANOPY_CPP_SOURCE_DIR}/rpc/include)

if(NOT CANOPY_BUILD_COROUTINE)
  add_executable(zone_address_test zone_address_test.cpp)

  target_link_libraries(zone_address_test GTest::gtest_main rpc::rpc rpc_types_idl)

  target_include_directories(
    zone_address_test PRIVATE ${CANOPY_CPP_SOURCE_DIR}/rpc/include ${CANOPY_CPP_SOURCE_DIR}/telemetry/include
                              ${CMAKE_BINARY_DIR}/generated/include ${CMAKE_BINARY_DIR}/generated/src)

  target_compile_options(zone_address_test PRIVATE ${CANOPY_COMPILE_OPTIONS})
  target_link_options(zone_address_test PRIVATE ${CANOPY_LINK_EXE_OPTIONS})

  add_test(NAME zone_address_test COMMAND zone_address_test)

  add_executable(zone_address_benchmark zone_address_benchmark.cpp)
  target_link_libraries(zone_address_benchmark rpc::rpc rpc_types_idl)
  target_include_directories(
    zone_address_benchmark PRIVATE ${CANOPY_CPP_SOURCE_DIR}/rpc/include ${CANOPY_CPP_SOURCE_DIR}/telemetry/include
                                   ${CMAKE_BINARY_DIR}/generated/include ${CMAKE_BINARY_DIR}/generated/src)

  target_compile_options(zone_address_benchmark PRIVATE ${CANOPY_COMPILE_OPTIONS})
  target_link_options(zone_address_benchmark PRIVATE ${CANOPY_LINK_EXE_OPTIONS})

  if(CANOPY_ENABLE_CLANG_TIDY)
    set_target_properties(zone_address_test PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
    set_target_properties(zone_address_benchmark PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
  endif()

  add_test(NAME zone_address_benchmark COMMAND zone_address_benchmark)

  include(CTest)
  if(NOT CANOPY_DEBUG_ADDRESS)
    gtest_discover_tests(zone_address_test)
  endif()
endif()
