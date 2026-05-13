#
# Copyright (c) 2026 Edward Boggis-Rolfe All rights reserved.
#

cmake_minimum_required(VERSION 3.24)

find_package(OpenSSL REQUIRED)

# Host OpenSSL backend. The SGXSSL handling below only exists to make the
# same source compile and link inside enclaves.
add_library(streaming_tls STATIC src/stream.cpp)
target_include_directories(
  streaming_tls
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include>"
  PRIVATE ${CANOPY_INCLUDES})
target_compile_definitions(streaming_tls PRIVATE ${CANOPY_DEFINES})
target_compile_options(streaming_tls PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(streaming_tls PRIVATE ${CANOPY_LINK_OPTIONS})
target_link_libraries(
  streaming_tls
  PUBLIC streaming
  PRIVATE OpenSSL::Crypto OpenSSL::SSL rpc libcoro)
if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(streaming_tls PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()

if(CANOPY_BUILD_ENCLAVE)
  canopy_configure_sgxssl_for_enclave()

  if(CANOPY_SGXSSL_HEADERS_READY)
    # Build the enclave variant from the same implementation file. OPENSSL_NO_STDIO
    # keeps OpenSSL away from host stdio APIs, and tsgxsslio.h supplies SGXSSL's
    # enclave-compatible BIO hooks for real SGX builds.
    add_library(streaming_tls_enclave STATIC src/stream.cpp)
    target_include_directories(
      streaming_tls_enclave
      PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include>"
      PRIVATE ${CANOPY_ENCLAVE_LIBCXX_INCLUDES} "${CANOPY_SGX_OPENSSL_INCLUDE_DIR}" "${CANOPY_SGXSSL_INCLUDE_DIR}")
    target_compile_definitions(streaming_tls_enclave PRIVATE ${CANOPY_ENCLAVE_DEFINES} OPENSSL_NO_STDIO)
    target_compile_options(streaming_tls_enclave PRIVATE ${CANOPY_ENCLAVE_COMPILE_OPTIONS} ${CANOPY_WARN_OK_ENCLAVE})
    if(NOT CANOPY_SGX_BACKEND STREQUAL "Fake")
      target_compile_options(streaming_tls_enclave PRIVATE "SHELL:-include tsgxsslio.h")
    endif()
    target_link_options(streaming_tls_enclave PRIVATE ${CANOPY_ENCLAVE_LINK_OPTIONS})
    target_link_libraries(
      streaming_tls_enclave
      PUBLIC streaming_enclave
      PRIVATE rpc::rpc_enclave)

    if(CANOPY_SGXSSL_IMPL_LIBS_FOUND)
      target_link_libraries(
        streaming_tls_enclave
        PRIVATE ${CANOPY_SGXSSL_ENCLAVE_LINK_LIBRARIES})
      if(TARGET canopy_prepare_sgxssl)
        add_dependencies(streaming_tls_enclave canopy_prepare_sgxssl)
      endif()
      message(STATUS "streaming_tls_enclave using ${CANOPY_SGXSSL_ARCHIVE_CONFIG} SGXSSL archives from "
                     "${CANOPY_SGXSSL_LIB_DIR}")
    endif()

    if(NOT CANOPY_SGXSSL_IMPL_LIBS_FOUND)
      # Expected for fake SGX and for developer machines that only need compile
      # coverage. Consumers that require real enclave TLS should check
      # CANOPY_STREAMING_TLS_ENCLAVE_HAS_SGXSSL_IMPL before relying on final link.
      message(
        STATUS "streaming_tls_enclave configured for compile-fit only: SGXSSL OpenSSL implementation archives "
               "for ${CANOPY_SGXSSL_ARCHIVE_CONFIG} builds were not found. Expected "
               "libsgx_tsgxssl${CANOPY_SGXSSL_ARCHIVE_SUFFIX}.a, "
               "libsgx_tsgxssl_ssl${CANOPY_SGXSSL_ARCHIVE_SUFFIX}.a, and "
               "libsgx_tsgxssl_crypto${CANOPY_SGXSSL_ARCHIVE_SUFFIX}.a.")
    endif()
    message(STATUS "streaming_tls_enclave configured")
  else()
    message(STATUS "streaming_tls_enclave not configured: SGX OpenSSL headers or tsgxsslio.h were not found. Set "
                   "CANOPY_SGX_OPENSSL_INCLUDE_DIR and CANOPY_SGXSSL_INCLUDE_DIR to enable it.")
  endif()
endif()
