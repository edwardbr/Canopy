#
# Copyright (c) 2026 Edward Boggis-Rolfe All rights reserved.
#

cmake_minimum_required(VERSION 3.24)

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
  PUBLIC streaming
  PRIVATE canopy_mbedtls_tls rpc libcoro)
if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(streaming_mbedtls PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()

if(CANOPY_BUILD_ENCLAVE)
  if(TARGET canopy_mbedtls_tls_enclave)
    set(STREAMING_MBEDTLS_ENCLAVE_CRYPTO_LIB canopy_mbedtls_tls_enclave)
  elseif(CANOPY_SGX_BACKEND STREQUAL "Fake")
    set(STREAMING_MBEDTLS_ENCLAVE_CRYPTO_LIB canopy_mbedtls_tls)
  endif()
endif()

if(CANOPY_BUILD_ENCLAVE AND STREAMING_MBEDTLS_ENCLAVE_CRYPTO_LIB)
  add_library(streaming_mbedtls_enclave STATIC src/stream.cpp)
  target_include_directories(
    streaming_mbedtls_enclave
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include>"
    PRIVATE ${CANOPY_ENCLAVE_LIBCXX_INCLUDES})
  target_compile_definitions(streaming_mbedtls_enclave PRIVATE ${CANOPY_ENCLAVE_DEFINES})
  target_compile_options(streaming_mbedtls_enclave PRIVATE ${CANOPY_ENCLAVE_COMPILE_OPTIONS} ${CANOPY_WARN_OK_ENCLAVE})
  target_link_options(streaming_mbedtls_enclave PRIVATE ${CANOPY_ENCLAVE_LINK_OPTIONS})
  target_link_libraries(
    streaming_mbedtls_enclave
    PUBLIC streaming_enclave
    PRIVATE ${STREAMING_MBEDTLS_ENCLAVE_CRYPTO_LIB} rpc::rpc_enclave)
  if(CANOPY_ENABLE_CLANG_TIDY)
    set_target_properties(streaming_mbedtls_enclave PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
  endif()
  message(STATUS "streaming_mbedtls_enclave configured")
elseif(CANOPY_BUILD_ENCLAVE)
  message(STATUS "streaming_mbedtls_enclave not configured: an enclave-built Mbed TLS target is still required")
endif()

message(STATUS "streaming_mbedtls configured")
