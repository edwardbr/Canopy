#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]
#
# Serialisation Benchmark CMakeLists.txt Round-trip throughput for all scalar, vector, array and composite types across
# all four encoding formats.
#

cmake_minimum_required(VERSION 3.24)
project(
  benchmarking_serialisation
  VERSION 1.0.0
  LANGUAGES CXX)

# test_types_idl is defined in tests/idls/ when CANOPY_BUILD_TEST=ON. When building benchmarking without tests, generate
# it here.
if(NOT TARGET test_types_idl)
  CanopyGenerate(
    test_types
    serialiser_test/test_types.idl
    ${CANOPY_SOURCE_DIR}/tests/idls
    ${CMAKE_BINARY_DIR}/generated
    ""
    yas_binary
    yas_compressed_binary
    yas_json
    protocol_buffers
    include_paths ${CANOPY_SOURCE_DIR}/tests/idls/.)
endif()

add_executable(serialisation_benchmark benchmark.cpp)
target_compile_definitions(serialisation_benchmark PRIVATE ${CANOPY_DEFINES})
target_include_directories(
  serialisation_benchmark
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated/include>"
         "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated/src>"
  PRIVATE ${CANOPY_INCLUDES})
target_link_libraries(
  serialisation_benchmark
  PUBLIC test_types_idl
         yas_common
         rpc::rpc
         fmt::fmt-header-only
         ${CANOPY_LIBRARIES}
         rpc_log)
target_compile_options(serialisation_benchmark PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(serialisation_benchmark PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
set_property(TARGET serialisation_benchmark PROPERTY COMPILE_PDB_NAME serialisation_benchmark)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(serialisation_benchmark PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()
