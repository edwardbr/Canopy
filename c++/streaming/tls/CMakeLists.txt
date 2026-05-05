#
# Copyright (c) 2026 Edward Boggis-Rolfe All rights reserved.
#

cmake_minimum_required(VERSION 3.24)

find_package(OpenSSL REQUIRED)

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
  set(CANOPY_SGXSSL_INCLUDE_DIR
      ""
      CACHE PATH "Directory containing tsgxsslio.h for enclave TLS stream builds")
  if(NOT CANOPY_SGXSSL_INCLUDE_DIR)
    foreach(_canopy_sgxssl_include_candidate
            "${CMAKE_SOURCE_DIR}/submodules/confidential-computing.sgx/SampleCode/SampleAttestedTLS/sgxssl/Linux/package/include"
            "${CMAKE_SOURCE_DIR}/submodules/confidential-computing.sgx/external/dcap_source/QuoteVerification/sgxssl/Linux/package/include"
            "${CMAKE_SOURCE_DIR}/submodules/confidential-computing.sgx/sdk/ttls")
      if(EXISTS "${_canopy_sgxssl_include_candidate}/tsgxsslio.h")
        set(CANOPY_SGXSSL_INCLUDE_DIR
            "${_canopy_sgxssl_include_candidate}"
            CACHE PATH "Directory containing tsgxsslio.h for enclave TLS stream builds" FORCE)
        break()
      endif()
    endforeach()
  endif()

  set(CANOPY_SGX_OPENSSL_INCLUDE_DIR
      ""
      CACHE PATH "Directory containing SGX-compatible OpenSSL headers for enclave TLS stream builds")
  if(NOT CANOPY_SGX_OPENSSL_INCLUDE_DIR)
    foreach(_canopy_sgx_openssl_include_candidate "${CANOPY_SGXSSL_INCLUDE_DIR}"
            "${CMAKE_SOURCE_DIR}/submodules/confidential-computing.sgx/external/dcap_source/prebuilt/openssl/inc")
      if(EXISTS "${_canopy_sgx_openssl_include_candidate}/openssl/ssl.h")
        set(CANOPY_SGX_OPENSSL_INCLUDE_DIR
            "${_canopy_sgx_openssl_include_candidate}"
            CACHE PATH "Directory containing SGX-compatible OpenSSL headers for enclave TLS stream builds" FORCE)
        break()
      endif()
    endforeach()
  endif()

  set(CANOPY_SGX_TLS_LIB_DIR
      ""
      CACHE PATH "Directory containing libsgx_ttls.a and libsgx_tcrypto.a for enclave TLS stream builds")
  if(NOT CANOPY_SGX_TLS_LIB_DIR)
    foreach(_canopy_sgx_tls_lib_candidate
            "${CMAKE_SOURCE_DIR}/submodules/confidential-computing.sgx/linux/installer/common/sdk/output/package/lib64"
            "${CMAKE_SOURCE_DIR}/submodules/confidential-computing.sgx/build/linux")
      if(EXISTS "${_canopy_sgx_tls_lib_candidate}/libsgx_ttls.a"
         AND EXISTS "${_canopy_sgx_tls_lib_candidate}/libsgx_tcrypto.a")
        set(CANOPY_SGX_TLS_LIB_DIR
            "${_canopy_sgx_tls_lib_candidate}"
            CACHE PATH "Directory containing libsgx_ttls.a and libsgx_tcrypto.a for enclave TLS stream builds" FORCE)
        break()
      endif()
    endforeach()
  endif()

  set(_canopy_sgxssl_archive_suffix "")
  set(_canopy_sgxssl_archive_config "release")
  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(_canopy_sgxssl_archive_suffix "d")
    set(_canopy_sgxssl_archive_config "debug")
  endif()

  set(CANOPY_SGXSSL_LIB_DIR
      ""
      CACHE PATH "Directory containing libsgx_tsgxssl*.a for enclave TLS stream builds")
  if(NOT CANOPY_SGXSSL_LIB_DIR)
    foreach(_canopy_sgxssl_lib_candidate
            "${CMAKE_SOURCE_DIR}/submodules/confidential-computing.sgx/SampleCode/SampleAttestedTLS/sgxssl/Linux/package/lib64"
            "${CMAKE_SOURCE_DIR}/submodules/confidential-computing.sgx/external/dcap_source/QuoteVerification/sgxssl/Linux/package/lib64")
      if(EXISTS "${_canopy_sgxssl_lib_candidate}/libsgx_tsgxssl${_canopy_sgxssl_archive_suffix}.a"
         AND EXISTS "${_canopy_sgxssl_lib_candidate}/libsgx_tsgxssl_ssl${_canopy_sgxssl_archive_suffix}.a"
         AND EXISTS "${_canopy_sgxssl_lib_candidate}/libsgx_tsgxssl_crypto${_canopy_sgxssl_archive_suffix}.a")
        set(CANOPY_SGXSSL_LIB_DIR
            "${_canopy_sgxssl_lib_candidate}"
            CACHE PATH "Directory containing libsgx_tsgxssl*.a for enclave TLS stream builds" FORCE)
        break()
      endif()
    endforeach()
  endif()

  if(EXISTS "${CANOPY_SGXSSL_INCLUDE_DIR}/tsgxsslio.h"
     AND EXISTS "${CANOPY_SGX_OPENSSL_INCLUDE_DIR}/openssl/ssl.h")
    add_library(streaming_tls_enclave STATIC src/stream.cpp)
    target_include_directories(
      streaming_tls_enclave
      PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include>"
      PRIVATE ${CANOPY_ENCLAVE_LIBCXX_INCLUDES} "${CANOPY_SGX_OPENSSL_INCLUDE_DIR}" "${CANOPY_SGXSSL_INCLUDE_DIR}")
    target_compile_definitions(streaming_tls_enclave PRIVATE ${CANOPY_ENCLAVE_DEFINES})
    target_compile_options(
      streaming_tls_enclave
      PRIVATE ${CANOPY_ENCLAVE_COMPILE_OPTIONS} ${CANOPY_WARN_OK_ENCLAVE} "SHELL:-include tsgxsslio.h")
    target_link_options(streaming_tls_enclave PRIVATE ${CANOPY_ENCLAVE_LINK_OPTIONS})
    target_link_libraries(streaming_tls_enclave PUBLIC streaming_enclave PRIVATE rpc::rpc_enclave libcoro_enclave)

    if(EXISTS "${CANOPY_SGX_TLS_LIB_DIR}/libsgx_ttls.a"
       AND EXISTS "${CANOPY_SGX_TLS_LIB_DIR}/libsgx_tcrypto.a")
      target_link_libraries(streaming_tls_enclave PRIVATE "${CANOPY_SGX_TLS_LIB_DIR}/libsgx_ttls.a"
                                                          "${CANOPY_SGX_TLS_LIB_DIR}/libsgx_tcrypto.a")
    endif()

    set(_canopy_sgxssl_lib "${CANOPY_SGXSSL_LIB_DIR}/libsgx_tsgxssl${_canopy_sgxssl_archive_suffix}.a")
    set(_canopy_sgxssl_ssl_lib "${CANOPY_SGXSSL_LIB_DIR}/libsgx_tsgxssl_ssl${_canopy_sgxssl_archive_suffix}.a")
    set(_canopy_sgxssl_crypto_lib
        "${CANOPY_SGXSSL_LIB_DIR}/libsgx_tsgxssl_crypto${_canopy_sgxssl_archive_suffix}.a")
    set(_canopy_sgxssl_impl_libs_found FALSE)
    if(EXISTS "${_canopy_sgxssl_lib}"
       AND EXISTS "${_canopy_sgxssl_ssl_lib}"
       AND EXISTS "${_canopy_sgxssl_crypto_lib}")
      set(_canopy_sgxssl_impl_libs_found TRUE)
      target_link_libraries(streaming_tls_enclave PRIVATE "-Wl,--whole-archive" "${_canopy_sgxssl_lib}"
                                                          "-Wl,--no-whole-archive" "${_canopy_sgxssl_ssl_lib}"
                                                          "${_canopy_sgxssl_crypto_lib}")
      message(STATUS "streaming_tls_enclave using ${_canopy_sgxssl_archive_config} SGXSSL archives from "
                     "${CANOPY_SGXSSL_LIB_DIR}")
    endif()

    if(NOT _canopy_sgxssl_impl_libs_found)
      message(
        STATUS
          "streaming_tls_enclave configured for compile-fit only: SGXSSL OpenSSL implementation archives "
          "for ${_canopy_sgxssl_archive_config} builds were not found. Expected "
          "libsgx_tsgxssl${_canopy_sgxssl_archive_suffix}.a, "
          "libsgx_tsgxssl_ssl${_canopy_sgxssl_archive_suffix}.a, and "
          "libsgx_tsgxssl_crypto${_canopy_sgxssl_archive_suffix}.a.")
    endif()
    message(STATUS "streaming_tls_enclave configured")
  else()
    message(
      STATUS
        "streaming_tls_enclave not configured: SGX OpenSSL headers or tsgxsslio.h were not found. Set "
        "CANOPY_SGX_OPENSSL_INCLUDE_DIR and CANOPY_SGXSSL_INCLUDE_DIR to enable it.")
  endif()
endif()
