#
# Copyright (c) 2026 Edward Boggis-Rolfe All rights reserved.
#

cmake_minimum_required(VERSION 3.24)

CanopyGenerate(
  tcp_coroutine_stream_config
  tcp_coroutine_stream/tcp_coroutine_stream_config.idl
  ${CMAKE_CURRENT_SOURCE_DIR}/interfaces
  ${CMAKE_BINARY_DIR}/generated
  ""
  yas_json
  dependencies rpc_types_idl io_uring_config_idl
  include_paths ${CMAKE_CURRENT_SOURCE_DIR}/interfaces ${CANOPY_CPP_SOURCE_DIR}/subcomponents/io_uring_config/interfaces
                ${CANOPY_INTERFACE_SOURCE_DIR}
  install_dir ${GENERATED_INSTALL_DIR})

if(NOT CANOPY_BUILD_COROUTINE
   OR NOT (CMAKE_SYSTEM_NAME STREQUAL "Linux")
   OR NOT TARGET canopy_io_uring)
  message(
    STATUS "streaming_tcp_coroutine implementation not configured: it requires coroutine Linux and canopy_io_uring")
  return()
endif()

set(STREAMING_TCP_COROUTINE_SOURCES src/acceptor.cpp src/connector.cpp src/stream.cpp)

add_library(streaming_tcp_coroutine STATIC ${STREAMING_TCP_COROUTINE_SOURCES} src/factory.cpp)
target_include_directories(
  streaming_tcp_coroutine
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include>"
  PRIVATE ${CANOPY_INCLUDES})
target_compile_definitions(streaming_tcp_coroutine PRIVATE ${CANOPY_DEFINES})
target_compile_options(streaming_tcp_coroutine PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(streaming_tcp_coroutine PRIVATE ${CANOPY_LINK_OPTIONS})
target_link_libraries(
  streaming_tcp_coroutine
  PUBLIC streaming
         transport_streaming
         canopy_io_uring
         io_uring_config_idl
         tcp_coroutine_stream_config_idl
  PRIVATE rpc libcoro)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(streaming_tcp_coroutine PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()

if(CANOPY_BUILD_TEST
   AND TARGET transport_streaming
   AND TARGET streaming_openssl_tls
   AND TARGET streaming_websocket)
  add_executable(tcp_coroutine_stream_composition_fit tests/stream_composition_fit.cpp)
  target_include_directories(tcp_coroutine_stream_composition_fit PRIVATE ${CANOPY_INCLUDES})
  target_compile_definitions(tcp_coroutine_stream_composition_fit PRIVATE ${CANOPY_DEFINES})
  target_compile_options(tcp_coroutine_stream_composition_fit PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
  target_link_options(tcp_coroutine_stream_composition_fit PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
  target_link_libraries(
    tcp_coroutine_stream_composition_fit
    PRIVATE streaming_tcp_coroutine
            streaming_openssl_tls
            streaming_websocket
            transport_streaming
            rpc_log)
  set_property(TARGET tcp_coroutine_stream_composition_fit PROPERTY COMPILE_PDB_NAME
                                                                    tcp_coroutine_stream_composition_fit)
  message(STATUS "tcp_coroutine_stream_composition_fit configured")
endif()
