# Copyright (c) 2026 Edward Boggis-Rolfe All rights reserved.

cmake_minimum_required(VERSION 3.24)

if(NOT CANOPY_BUILD_COROUTINE)
  add_executable(blocking_tcp_stream_test blocking_tcp_stream_test.cpp)

  target_link_libraries(blocking_tcp_stream_test PRIVATE GTest::gtest_main rpc::rpc streaming streaming_tcp rpc_log)

  target_include_directories(
    blocking_tcp_stream_test PRIVATE ${CANOPY_CPP_SOURCE_DIR}/rpc/include
                                     ${CMAKE_BINARY_DIR}/generated/include)

  target_compile_options(blocking_tcp_stream_test PRIVATE ${CANOPY_COMPILE_OPTIONS})
  target_link_options(blocking_tcp_stream_test PRIVATE ${CANOPY_LINK_EXE_OPTIONS})

  add_test(NAME blocking_tcp_stream_test COMMAND blocking_tcp_stream_test)

  if(CANOPY_ENABLE_CLANG_TIDY)
    set_target_properties(blocking_tcp_stream_test PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
  endif()
endif()
