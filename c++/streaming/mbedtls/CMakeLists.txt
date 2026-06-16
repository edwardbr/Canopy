#
# Copyright (c) 2026 Edward Boggis-Rolfe All rights reserved.
#

cmake_minimum_required(VERSION 3.24)

CanopyGenerate(
  mbedtls_stream_config
  mbedtls_stream/mbedtls_stream_config.idl
  ${CMAKE_CURRENT_SOURCE_DIR}/interfaces
  ${CMAKE_BINARY_DIR}/generated
  ""
  yas_json
  dependencies rpc_types_idl
  include_paths ${CMAKE_CURRENT_SOURCE_DIR}/interfaces ${CANOPY_INTERFACE_SOURCE_DIR}
  install_dir ${GENERATED_INSTALL_DIR})

add_library(streaming_mbedtls STATIC src/stream.cpp)
target_include_directories(
  streaming_mbedtls
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include>"
  PRIVATE ${CANOPY_INCLUDES})
target_compile_definitions(streaming_mbedtls PRIVATE ${CANOPY_DEFINES})
target_compile_options(streaming_mbedtls PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(streaming_mbedtls PRIVATE ${CANOPY_LINK_OPTIONS})
target_link_libraries(
  streaming_mbedtls
  PUBLIC streaming mbedtls_stream_config_idl
  PRIVATE canopy_mbedtls_tls rpc libcoro)
if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(streaming_mbedtls PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()

message(STATUS "streaming_mbedtls configured")
