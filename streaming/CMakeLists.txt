#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

find_package(OpenSSL REQUIRED)

add_library(
  streaming SHARED
  src/stream.cpp
  src/tcp_stream.cpp
  src/tls_stream.cpp
  src/ws_stream.cpp
  src/spsc_queue_stream.cpp)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_sources(streaming PRIVATE src/io_uring_tcp_stream.cpp)
endif()

target_include_directories(
  streaming
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  PRIVATE ${CANOPY_INCLUDES})

target_compile_definitions(streaming PRIVATE ${CANOPY_DEFINES})
if(CANOPY_IO_URING_SQPOLL)
  target_compile_definitions(streaming PRIVATE CANOPY_IO_URING_SQPOLL)
endif()
target_compile_options(streaming PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(streaming PRIVATE ${CANOPY_LINK_EXE_OPTIONS})

target_link_libraries(
  streaming
  PUBLIC libcoro
         OpenSSL::Crypto
         OpenSSL::SSL
         wslay
         rpc
         canopy_network_config
         spsc_queue)

set_property(TARGET streaming PROPERTY COMPILE_PDB_NAME streaming)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(streaming PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()

if(CANOPY_BUILD_TEST
   AND CMAKE_SYSTEM_NAME STREQUAL "Linux"
   AND CANOPY_BUILD_COROUTINE)
  add_executable(io_uring_stream_test tests/io_uring_stream_test.cpp)
  target_include_directories(io_uring_stream_test PRIVATE ${CANOPY_INCLUDES})
  target_compile_definitions(io_uring_stream_test PRIVATE ${CANOPY_DEFINES})
  target_compile_options(io_uring_stream_test PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
  target_link_options(io_uring_stream_test PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
  target_link_libraries(io_uring_stream_test PRIVATE streaming GTest::gtest_main)
  add_test(NAME io_uring_stream_test COMMAND io_uring_stream_test)
  message(STATUS "io_uring_stream_test configured")
endif()

message(STATUS "streaming configured")
