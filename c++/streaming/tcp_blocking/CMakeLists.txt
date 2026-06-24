#
# Copyright (c) 2026 Edward Boggis-Rolfe All rights reserved.
#

cmake_minimum_required(VERSION 3.24)

CanopyGenerate(
  tcp_blocking_stream_config
  tcp_blocking_stream/tcp_blocking_stream_config.idl
  ${CMAKE_CURRENT_SOURCE_DIR}/interfaces
  ${CMAKE_BINARY_DIR}/generated
  ""
  yas_json
  dependencies rpc_types_idl
  include_paths ${CMAKE_CURRENT_SOURCE_DIR}/interfaces ${CANOPY_INTERFACE_SOURCE_DIR}
  install_dir ${GENERATED_INSTALL_DIR})

if(CANOPY_BUILD_COROUTINE)
  message(STATUS "streaming_tcp_blocking implementation not configured: blocking TCP is disabled in coroutine builds")
  return()
endif()

add_library(streaming_tcp_blocking STATIC src/stream.cpp src/socket.cpp src/acceptor.cpp src/factory.cpp)
target_include_directories(
  streaming_tcp_blocking
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include>"
  PRIVATE ${CANOPY_INCLUDES})
target_compile_definitions(streaming_tcp_blocking PRIVATE ${CANOPY_DEFINES})
target_compile_options(streaming_tcp_blocking PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(streaming_tcp_blocking PRIVATE ${CANOPY_LINK_OPTIONS})
target_link_libraries(
  streaming_tcp_blocking
  PUBLIC streaming tcp_blocking_stream_config_idl
  PRIVATE rpc canopy_dns_resolver)
if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(streaming_tcp_blocking PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()
