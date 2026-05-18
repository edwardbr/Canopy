#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

find_package(OpenSSL REQUIRED)

set(CANOPY_SECURITY_ATTESTATION_SOURCES
    src/aead.cpp
    src/backend_factory.cpp
    src/crypto_hash.cpp
    src/fake_backend.cpp
    src/kdf.cpp
    src/null_backend.cpp
    src/protected_rpc.cpp
    src/simulation_backend.cpp
    src/service.cpp)

add_library(security_attestation STATIC ${CANOPY_SECURITY_ATTESTATION_SOURCES})
add_library(canopy::security_attestation ALIAS security_attestation)

target_include_directories(
  security_attestation
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include>"
  PRIVATE ${CANOPY_INCLUDES})
target_compile_definitions(security_attestation PRIVATE ${CANOPY_DEFINES})
target_compile_options(security_attestation PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(security_attestation PRIVATE ${CANOPY_LINK_OPTIONS})
target_link_libraries(
  security_attestation PUBLIC rpc PRIVATE protected_rpc_protocol_idl sgx_sim_protocol_idl OpenSSL::Crypto)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(security_attestation PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()

if(CANOPY_BUILD_ENCLAVE)
  if(CANOPY_SGX_BACKEND STREQUAL "Fake")
    set(CANOPY_ATTESTATION_ENCLAVE_HEADERS_READY TRUE)
  else()
    canopy_configure_sgxssl_for_enclave()
    set(CANOPY_ATTESTATION_ENCLAVE_HEADERS_READY ${CANOPY_SGXSSL_HEADERS_READY})
  endif()

  if(CANOPY_ATTESTATION_ENCLAVE_HEADERS_READY)
    add_library(security_attestation_enclave STATIC ${CANOPY_SECURITY_ATTESTATION_SOURCES})
    add_library(canopy::security_attestation_enclave ALIAS security_attestation_enclave)

    target_include_directories(
      security_attestation_enclave
      PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include>"
      PRIVATE ${CANOPY_ENCLAVE_LIBCXX_INCLUDES})
    target_compile_definitions(security_attestation_enclave PRIVATE ${CANOPY_ENCLAVE_DEFINES})
    target_compile_options(security_attestation_enclave PRIVATE ${CANOPY_ENCLAVE_COMPILE_OPTIONS}
                                                               ${CANOPY_WARN_OK_ENCLAVE})
    if(NOT CANOPY_SGX_BACKEND STREQUAL "Fake")
      target_include_directories(
        security_attestation_enclave PRIVATE "${CANOPY_SGX_OPENSSL_INCLUDE_DIR}" "${CANOPY_SGXSSL_INCLUDE_DIR}")
      target_compile_definitions(security_attestation_enclave PRIVATE OPENSSL_NO_STDIO)
      target_compile_options(security_attestation_enclave PRIVATE "SHELL:-include tsgxsslio.h")
    endif()
    target_link_options(security_attestation_enclave PRIVATE ${CANOPY_ENCLAVE_LINK_OPTIONS})
    target_link_libraries(
      security_attestation_enclave PUBLIC rpc::rpc_enclave PRIVATE protected_rpc_protocol_idl_enclave
                                                   sgx_sim_protocol_idl_enclave)

    if(CANOPY_SGX_BACKEND STREQUAL "Fake")
      target_link_libraries(security_attestation_enclave PRIVATE OpenSSL::Crypto)
      message(STATUS "security_attestation_enclave configured for fake SGX using host OpenSSL::Crypto")
    endif()

    if(NOT CANOPY_SGX_BACKEND STREQUAL "Fake" AND CANOPY_SGXSSL_IMPL_LIBS_FOUND)
      target_link_libraries(security_attestation_enclave PRIVATE ${CANOPY_SGXSSL_ENCLAVE_LINK_LIBRARIES})
      if(TARGET canopy_prepare_sgxssl)
        add_dependencies(security_attestation_enclave canopy_prepare_sgxssl)
      endif()
      message(STATUS "security_attestation_enclave using ${CANOPY_SGXSSL_ARCHIVE_CONFIG} SGXSSL archives from "
                     "${CANOPY_SGXSSL_LIB_DIR}")
    endif()

    if(NOT CANOPY_SGX_BACKEND STREQUAL "Fake" AND NOT CANOPY_SGXSSL_IMPL_LIBS_FOUND)
      message(
        STATUS "security_attestation_enclave configured for compile-fit only: SGXSSL OpenSSL implementation archives "
               "for ${CANOPY_SGXSSL_ARCHIVE_CONFIG} builds were not found.")
    endif()

    if(CANOPY_ENABLE_CLANG_TIDY)
      set_target_properties(security_attestation_enclave PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
    endif()

    message(STATUS "security_attestation_enclave configured")
  else()
    message(STATUS "security_attestation_enclave not configured: SGX OpenSSL headers or tsgxsslio.h were not found. "
                   "Set CANOPY_SGX_OPENSSL_INCLUDE_DIR and CANOPY_SGXSSL_INCLUDE_DIR to enable it.")
  endif()
endif()

if(CANOPY_BUILD_TEST)
  add_executable(attestation_service_test tests/attestation_service_test.cpp)
  target_include_directories(attestation_service_test PRIVATE ${CANOPY_INCLUDES})
  target_compile_definitions(attestation_service_test PRIVATE ${CANOPY_DEFINES})
  target_compile_options(attestation_service_test PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
  target_link_options(attestation_service_test PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
  target_link_libraries(
    attestation_service_test PRIVATE canopy::security_attestation route_attestation_protocol_idl GTest::gtest_main
                                     rpc_log)
  add_test(NAME attestation_service_test COMMAND attestation_service_test)
  message(STATUS "attestation_service_test configured")
endif()

message(STATUS "security_attestation configured")
