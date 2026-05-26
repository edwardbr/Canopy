#
# Copyright (c) 2026 Edward Boggis-Rolfe All rights reserved.
#

cmake_minimum_required(VERSION 3.24)

add_library(streaming_tcp STATIC src/stream.cpp src/socket.cpp src/acceptor.cpp)
target_include_directories(
  streaming_tcp
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include>"
  PRIVATE ${CANOPY_INCLUDES})
target_compile_definitions(streaming_tcp PRIVATE ${CANOPY_DEFINES})
target_compile_options(streaming_tcp PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(streaming_tcp PRIVATE ${CANOPY_LINK_OPTIONS})
if(CANOPY_BUILD_COROUTINE)
  target_link_libraries(
    streaming_tcp
    PUBLIC streaming libcoro canopy_cares
    PRIVATE rpc)
else()
  # Blocking-mode TCP uses POSIX sockets directly; no libcoro/c-ares.
  target_link_libraries(
    streaming_tcp
    PUBLIC streaming
    PRIVATE rpc)
endif()
if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(streaming_tcp PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()
