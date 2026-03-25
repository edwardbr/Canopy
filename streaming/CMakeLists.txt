#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

add_subdirectory(core)
add_subdirectory(tcp)
add_subdirectory(tls)
add_subdirectory(spsc_queue)
add_subdirectory(spsc_wrapping)

if(CANOPY_BUILD_WEBSOCKET)
  add_subdirectory(websocket)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # io_uring and io_uring_tcp are INTERFACE / header-only targets; they impose no build cost and are excluded on
  # non-Linux because io_uring is a Linux kernel API. They do not require CANOPY_BUILD_COROUTINE here because the
  # coroutine guard is applied at the consumer level (transport targets and io_uring_stream_test below).
  add_subdirectory(io_uring)
  add_subdirectory(io_uring_tcp)
endif()

if(CANOPY_BUILD_TEST
   AND CMAKE_SYSTEM_NAME STREQUAL "Linux"
   AND CANOPY_BUILD_COROUTINE)
  add_executable(io_uring_stream_test tests/io_uring_stream_test.cpp)
  target_include_directories(io_uring_stream_test PRIVATE ${CANOPY_INCLUDES})
  target_compile_definitions(io_uring_stream_test PRIVATE ${CANOPY_DEFINES})
  target_compile_options(io_uring_stream_test PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
  target_link_options(io_uring_stream_test PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
  target_link_libraries(io_uring_stream_test PRIVATE streaming streaming_io_uring GTest::gtest_main rpc_log)
  add_test(NAME io_uring_stream_test COMMAND io_uring_stream_test)
  message(STATUS "io_uring_stream_test configured")

  add_executable(streaming_regression_test tests/streaming_regression_test.cpp)
  target_include_directories(streaming_regression_test PRIVATE ${CANOPY_INCLUDES})
  target_compile_definitions(streaming_regression_test PRIVATE ${CANOPY_DEFINES})
  target_compile_options(streaming_regression_test PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
  target_link_options(streaming_regression_test PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
  target_link_libraries(streaming_regression_test PRIVATE streaming streaming_spsc_wrapping GTest::gtest_main rpc_log)
  add_test(NAME streaming_regression_test COMMAND streaming_regression_test)
  message(STATUS "streaming_regression_test configured")
endif()

message(STATUS "streaming configured")
