#
# Copyright (c) 2026 Edward Boggis-Rolfe All rights reserved.
#

cmake_minimum_required(VERSION 3.24)

add_library(streaming_websocket STATIC src/stream.cpp)
target_include_directories(
  streaming_websocket
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  PRIVATE ${CANOPY_INCLUDES})
target_compile_definitions(streaming_websocket PRIVATE ${CANOPY_DEFINES})
target_compile_options(streaming_websocket PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(streaming_websocket PRIVATE ${CANOPY_LINK_OPTIONS})
target_link_libraries(
  streaming_websocket
  PUBLIC streaming wslay
  PRIVATE rpc libcoro)
if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(streaming_websocket PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()
