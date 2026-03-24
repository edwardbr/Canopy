#
# Copyright (c) 2026 Edward Boggis-Rolfe All rights reserved.
#

cmake_minimum_required(VERSION 3.24)

add_library(streaming_io_uring_tcp STATIC src/stream.cpp)
target_include_directories(
  streaming_io_uring_tcp
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  PRIVATE ${CANOPY_INCLUDES})
target_compile_definitions(streaming_io_uring_tcp PRIVATE ${CANOPY_DEFINES})
target_compile_options(streaming_io_uring_tcp PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(streaming_io_uring_tcp PRIVATE ${CANOPY_LINK_OPTIONS})
target_link_libraries(
  streaming_io_uring_tcp
  PUBLIC streaming
  PRIVATE rpc libcoro)
if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(streaming_io_uring_tcp PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()
