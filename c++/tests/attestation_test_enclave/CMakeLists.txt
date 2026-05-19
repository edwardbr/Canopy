#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]
cmake_minimum_required(VERSION 3.24)

if(CANOPY_BUILD_ENCLAVE AND CANOPY_BUILD_COROUTINE)
  set(ATTESTATION_TEST_TELEMETRY_ENCLAVE_LIBS)
  if(CANOPY_USE_TELEMETRY)
    list(APPEND ATTESTATION_TEST_TELEMETRY_ENCLAVE_LIBS rpc_telemetry_enclave)
  endif()

  add_enclave_library(
    sgx_attestation_test_enclave
    SRCS attestation_test_enclave.cpp
    HEADER_ONLY_LIBS ${CANOPY_ENCLAVE_FMT_LIB}
    TRUSTED_LIBS
      libcoro_enclave
      rpc_enclave
      transport_sgx_coroutine_enclave
      ${ATTESTATION_TEST_TELEMETRY_ENCLAVE_LIBS}
      canopy_coroutine_edl_enclave
      common_enclave
      attestation_test_idl_enclave
      sgx_sim_protocol_idl_enclave
      example_idl_enclave
      canopy::security_attestation_enclave
      yas_common
      ${SGX_ENCLAVE_LIBS})

  target_compile_definitions(sgx_attestation_test_enclave PRIVATE ${CANOPY_ENCLAVE_DEFINES})
  target_include_directories(
    sgx_attestation_test_enclave
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../common/include>"
           "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated/include>"
           "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated/src>"
           "$<INSTALL_INTERFACE:include/marshalled_tests>"
    PRIVATE ${CANOPY_ENCLAVE_LIBCXX_INCLUDES})

  if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(EXTRA_ATTESTATION_TEST_ENCLAVE_COMPILE_OPTIONS -Wno-unused-parameter)
  endif()

  target_compile_options(
    sgx_attestation_test_enclave PRIVATE ${CANOPY_ENCLAVE_COMPILE_OPTIONS} ${CANOPY_WARN_OK_ENCLAVE}
                                         ${EXTRA_ATTESTATION_TEST_ENCLAVE_COMPILE_OPTIONS})
  target_link_options(sgx_attestation_test_enclave PRIVATE ${CANOPY_ENCLAVE_LINK_OPTIONS})

  set_property(TARGET sgx_attestation_test_enclave PROPERTY COMPILE_PDB_NAME sgx_attestation_test_enclave)
  if(CANOPY_ENABLE_CLANG_TIDY)
    set_target_properties(sgx_attestation_test_enclave PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
  endif()

  enclave_sign(
    sgx_attestation_test_enclave
    KEY ../test_enclave/marshal_test_enclave_private.pem
    CONFIG ../test_enclave/marshal_test_enclave.config.${SGX_MODE}.xml)
endif()
