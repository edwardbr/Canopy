# Copyright (c) 2026 Edward Boggis-Rolfe All rights reserved.

cmake_minimum_required(VERSION 3.24)

if(NOT CANOPY_BUILD_COROUTINE)
  add_executable(blocking_listener_test blocking_listener_test.cpp)

  target_link_libraries(
    blocking_listener_test
    PRIVATE GTest::gtest_main
            rpc::rpc
            streaming
            streaming_tcp_blocking
            rpc_log)

  target_include_directories(blocking_listener_test PRIVATE ${CANOPY_CPP_SOURCE_DIR}/rpc/include
                                                            ${CMAKE_BINARY_DIR}/generated/include)

  target_compile_options(blocking_listener_test PRIVATE ${CANOPY_COMPILE_OPTIONS})
  target_link_options(blocking_listener_test PRIVATE ${CANOPY_LINK_EXE_OPTIONS})

  add_test(NAME blocking_listener_test COMMAND blocking_listener_test)
endif()
