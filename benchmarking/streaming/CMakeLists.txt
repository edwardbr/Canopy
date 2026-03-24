#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]
#
# Streaming Benchmark CMakeLists.txt Measures raw stream throughput and round-trip latency for every stream type: spsc,
# tcp, io_uring (Linux), tls+spsc, ws+spsc, tls+ws+spsc No transport or service layer is exercised.
#

cmake_minimum_required(VERSION 3.24)
project(
  benchmarking_streaming
  VERSION 1.0.0
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenSSL REQUIRED)

add_executable(streaming_benchmark benchmark.cpp)
target_compile_definitions(streaming_benchmark PRIVATE ${CANOPY_DEFINES})
target_include_directories(streaming_benchmark PRIVATE ${CANOPY_INCLUDES})
target_link_libraries(
  streaming_benchmark
  PUBLIC streaming
         streaming_tcp
         streaming_tls
         streaming_spsc_queue
         rpc
         OpenSSL::SSL
         OpenSSL::Crypto
         fmt::fmt-header-only
         ${CANOPY_LIBRARIES})

if(CANOPY_BUILD_WEBSOCKET)
  target_link_libraries(streaming_benchmark PUBLIC streaming_websocket)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(streaming_benchmark PUBLIC streaming_io_uring)
endif()
target_compile_options(streaming_benchmark PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(streaming_benchmark PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
set_property(TARGET streaming_benchmark PROPERTY COMPILE_PDB_NAME streaming_benchmark)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(streaming_benchmark PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()
