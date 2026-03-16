#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]
#
# Fullstack Benchmark CMakeLists.txt
# Benchmarks covering SPSC and TCP transport with multiple encodings
#

cmake_minimum_required(VERSION 3.24)
project(
  benchmarking_fullstack
  VERSION 1.0.0
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(benchmark benchmark.cpp)
target_compile_definitions(benchmark PRIVATE ${CANOPY_DEFINES})
target_include_directories(
  benchmark
  PRIVATE "${CMAKE_SOURCE_DIR}/demos/comprehensive/include" ${CANOPY_INCLUDES})
target_link_libraries(
  benchmark
  PUBLIC comprehensive_idl
         transport_local
         rpc
         ${CANOPY_LIBRARIES})
if(CANOPY_BUILD_COROUTINE)
  target_link_libraries(benchmark PUBLIC transport_streaming)
endif()
target_compile_options(benchmark PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(benchmark PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
set_property(TARGET benchmark PROPERTY COMPILE_PDB_NAME benchmark)
