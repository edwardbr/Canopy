#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

set(CANOPY_HTTP_SERVER_SECURE_STREAM_TARGET)
set(CANOPY_HTTP_SERVER_CRYPTO_LIBS)
set(CANOPY_HTTP_SERVER_TCP_TARGET)

CanopyGenerate(
  http_server_config
  http_server/http_server_config.idl
  ${CMAKE_CURRENT_SOURCE_DIR}/interfaces
  ${CMAKE_BINARY_DIR}/generated
  ""
  yas_json
  include_paths ${CMAKE_CURRENT_SOURCE_DIR}/interfaces ${CANOPY_INTERFACE_SOURCE_DIR}
  install_dir ${GENERATED_INSTALL_DIR})

if(CANOPY_BUILD_COROUTINE)
  if(NOT TARGET streaming_tcp_coroutine)
    message(FATAL_ERROR "canopy_http_server requires streaming_tcp_coroutine in coroutine builds")
  endif()
  set(CANOPY_HTTP_SERVER_TCP_TARGET streaming_tcp_coroutine)
else()
  if(NOT TARGET streaming_tcp_blocking)
    message(FATAL_ERROR "canopy_http_server requires streaming_tcp_blocking in blocking builds")
  endif()
  set(CANOPY_HTTP_SERVER_TCP_TARGET streaming_tcp_blocking)
endif()

# OpenSSL TLS is dual-mode (streaming_openssl_tls drives memory BIOs over the dual-mode streaming::stream interface). mbedtls is
# still coroutine-only.
if(CANOPY_SECURE_STREAM_BACKEND STREQUAL "MBEDTLS")
  if(NOT CANOPY_BUILD_COROUTINE)
    message(
      FATAL_ERROR "canopy_http_server requires OpenSSL for non-coroutine builds; mbedtls streams are coroutine-only")
  endif()
  if(NOT TARGET streaming_mbedtls)
    message(FATAL_ERROR "canopy_http_server requires streaming_mbedtls when CANOPY_SECURE_STREAM_BACKEND=MBEDTLS")
  endif()
  set(CANOPY_HTTP_SERVER_SECURE_STREAM_TARGET streaming_mbedtls)
  set(CANOPY_HTTP_SERVER_CRYPTO_LIBS canopy_mbedtls_crypto)
elseif(CANOPY_SECURE_STREAM_BACKEND STREQUAL "OPENSSL")
  find_package(OpenSSL REQUIRED)
  if(NOT TARGET streaming_openssl_tls)
    message(FATAL_ERROR "canopy_http_server requires streaming_openssl_tls when CANOPY_SECURE_STREAM_BACKEND=OPENSSL")
  endif()
  set(CANOPY_HTTP_SERVER_SECURE_STREAM_TARGET streaming_openssl_tls)
  set(CANOPY_HTTP_SERVER_CRYPTO_LIBS OpenSSL::SSL OpenSSL::Crypto)
else()
  message(FATAL_ERROR "Unsupported CANOPY_SECURE_STREAM_BACKEND=${CANOPY_SECURE_STREAM_BACKEND}")
endif()

add_library(canopy_http_server STATIC src/http_acceptor.cpp src/http_client_connection.cpp
                                      src/static_webpage_delivery.cpp)

target_include_directories(
  canopy_http_server
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include>"
  PRIVATE ${CANOPY_INCLUDES})

target_compile_definitions(canopy_http_server PRIVATE ${CANOPY_DEFINES})
target_compile_options(canopy_http_server PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(canopy_http_server PRIVATE ${CANOPY_LINK_EXE_OPTIONS})

target_link_libraries(
  canopy_http_server
  PUBLIC streaming
         http_server_config_idl
         ${CANOPY_HTTP_SERVER_SECURE_STREAM_TARGET}
         transport_streaming
         llhttp_static
         rpc
  PRIVATE ${CANOPY_HTTP_SERVER_TCP_TARGET}
          streaming_websocket
          ${CANOPY_HTTP_SERVER_CRYPTO_LIBS}
          ${CANOPY_FMT_LIB}
          ${CANOPY_LIBRARIES})

set_property(TARGET canopy_http_server PROPERTY COMPILE_PDB_NAME canopy_http_server)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(canopy_http_server PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()

set(CANOPY_HTTP_SERVER_ENCLAVE_SECURE_STREAM_TARGET)
set(CANOPY_HTTP_SERVER_ENCLAVE_CRYPTO_LIBS)
if(CANOPY_SECURE_STREAM_BACKEND STREQUAL "MBEDTLS")
  if(TARGET streaming_mbedtls_enclave)
    set(CANOPY_HTTP_SERVER_ENCLAVE_SECURE_STREAM_TARGET streaming_mbedtls_enclave)
    if(TARGET canopy_mbedtls_crypto_enclave)
      set(CANOPY_HTTP_SERVER_ENCLAVE_CRYPTO_LIBS canopy_mbedtls_crypto_enclave)
    else()
      set(CANOPY_HTTP_SERVER_ENCLAVE_CRYPTO_LIBS canopy_mbedtls_crypto)
    endif()
  endif()
elseif(TARGET streaming_openssl_tls_enclave)
  set(CANOPY_HTTP_SERVER_ENCLAVE_SECURE_STREAM_TARGET streaming_openssl_tls_enclave)
endif()

if(CANOPY_BUILD_ENCLAVE
   AND TARGET http_server_config_idl_enclave
   AND TARGET llhttp_enclave
   AND TARGET streaming_websocket_enclave
   AND CANOPY_HTTP_SERVER_ENCLAVE_SECURE_STREAM_TARGET)
  add_library(canopy_http_server_enclave STATIC src/http_client_connection.cpp src/static_webpage_delivery.cpp)

  target_include_directories(
    canopy_http_server_enclave
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include>"
    PRIVATE ${CANOPY_ENCLAVE_LIBCXX_INCLUDES} "${CANOPY_SGX_OPENSSL_INCLUDE_DIR}")

  target_compile_definitions(canopy_http_server_enclave PRIVATE ${CANOPY_ENCLAVE_DEFINES} OPENSSL_NO_STDIO)
  target_compile_options(canopy_http_server_enclave PRIVATE ${CANOPY_ENCLAVE_COMPILE_OPTIONS} ${CANOPY_WARN_OK_ENCLAVE})
  target_link_options(canopy_http_server_enclave PRIVATE ${CANOPY_ENCLAVE_LINK_OPTIONS})

  target_link_libraries(
    canopy_http_server_enclave
    PUBLIC streaming_enclave
           ${CANOPY_HTTP_SERVER_ENCLAVE_SECURE_STREAM_TARGET}
           http_server_config_idl_enclave
           transport_streaming_enclave
           llhttp_enclave
           rpc::rpc_enclave
    PRIVATE streaming_websocket_enclave ${CANOPY_HTTP_SERVER_ENCLAVE_CRYPTO_LIBS} ${CANOPY_ENCLAVE_FMT_LIB})

  if(CANOPY_ENABLE_CLANG_TIDY)
    set_target_properties(canopy_http_server_enclave PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
  endif()

  message(STATUS "canopy_http_server_enclave configured")
endif()

message(STATUS "canopy_http_server configured")
