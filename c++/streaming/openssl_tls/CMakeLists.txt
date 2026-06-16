#
# Copyright (c) 2026 Edward Boggis-Rolfe All rights reserved.
#

cmake_minimum_required(VERSION 3.24)

find_package(OpenSSL REQUIRED)

CanopyGenerate(
  openssl_tls_stream_config
  openssl_tls_stream/openssl_tls_stream_config.idl
  ${CMAKE_CURRENT_SOURCE_DIR}/interfaces
  ${CMAKE_BINARY_DIR}/generated
  ""
  yas_json
  dependencies rpc_types_idl
  include_paths ${CMAKE_CURRENT_SOURCE_DIR}/interfaces ${CANOPY_INTERFACE_SOURCE_DIR}
  install_dir ${GENERATED_INSTALL_DIR})

add_library(streaming_openssl_tls STATIC src/stream.cpp)
target_include_directories(
  streaming_openssl_tls
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include>"
  PRIVATE ${CANOPY_INCLUDES})
target_compile_definitions(streaming_openssl_tls PRIVATE ${CANOPY_DEFINES})
target_compile_options(streaming_openssl_tls PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(streaming_openssl_tls PRIVATE ${CANOPY_LINK_OPTIONS})
if(CANOPY_BUILD_COROUTINE)
  target_link_libraries(
    streaming_openssl_tls
    PUBLIC streaming openssl_tls_stream_config_idl
    PRIVATE OpenSSL::Crypto OpenSSL::SSL rpc libcoro)
else()
  # Blocking-mode TLS drives memory BIOs over a dual-mode streaming::stream; no libcoro dependency.
  target_link_libraries(
    streaming_openssl_tls
    PUBLIC streaming openssl_tls_stream_config_idl
    PRIVATE OpenSSL::Crypto OpenSSL::SSL rpc)
endif()
if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(streaming_openssl_tls PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()
