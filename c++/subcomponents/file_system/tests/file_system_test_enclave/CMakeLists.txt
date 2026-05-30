#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

if(CANOPY_BUILD_ENCLAVE AND CANOPY_BUILD_COROUTINE)
  set(FILE_SYSTEM_TEST_TELEMETRY_ENCLAVE_LIBS)
  if(CANOPY_USE_TELEMETRY)
    list(APPEND FILE_SYSTEM_TEST_TELEMETRY_ENCLAVE_LIBS rpc_telemetry_enclave)
  endif()

  set(FILE_SYSTEM_TEST_PROTOBUF_ENCLAVE_LIBS)
  if(TARGET canopy_nanopb_runtime_enclave)
    list(APPEND FILE_SYSTEM_TEST_PROTOBUF_ENCLAVE_LIBS canopy_nanopb_runtime_enclave)
  endif()
  if(TARGET rpc_types_idl_enclave AND (CANOPY_BUILD_PROTOCOL_BUFFERS OR CANOPY_BUILD_NANOPB))
    list(APPEND FILE_SYSTEM_TEST_PROTOBUF_ENCLAVE_LIBS rpc_types_idl_enclave)
  endif()

  add_enclave_library(
    file_system_test_enclave
    SRCS file_system_test_enclave.cpp
    HEADER_ONLY_LIBS ${CANOPY_ENCLAVE_FMT_LIB}
    TRUSTED_LIBS
      libcoro_enclave
      rpc_enclave
      streaming_enclave
      transport_streaming_enclave
      streaming_transport_idl_enclave
      transport_sgx_coroutine_enclave
      ${FILE_SYSTEM_TEST_TELEMETRY_ENCLAVE_LIBS}
      streaming_spsc_queue_enclave
      sgx_coroutine_transport_edl_enclave
      transport_local_enclave
      canopy_io_uring_enclave
      canopy_file_system_enclave
      file_system_idl_enclave
      yas_common
      ${FILE_SYSTEM_TEST_PROTOBUF_ENCLAVE_LIBS}
      ${SGX_ENCLAVE_LIBS})

  target_compile_definitions(file_system_test_enclave PRIVATE ${CANOPY_ENCLAVE_DEFINES})
  target_include_directories(
    file_system_test_enclave
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated/include>"
           "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated/src>"
    PRIVATE ${CANOPY_ENCLAVE_LIBCXX_INCLUDES})
  target_compile_options(file_system_test_enclave PRIVATE ${CANOPY_ENCLAVE_COMPILE_OPTIONS} ${CANOPY_WARN_OK_ENCLAVE})
  target_link_options(file_system_test_enclave PRIVATE ${CANOPY_ENCLAVE_LINK_OPTIONS})
  set_property(TARGET file_system_test_enclave PROPERTY COMPILE_PDB_NAME file_system_test_enclave)

  if(CANOPY_ENABLE_CLANG_TIDY)
    set_target_properties(file_system_test_enclave PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
  endif()

  enclave_sign(
    file_system_test_enclave
    KEY ../../../../tests/test_enclave/marshal_test_enclave_private.pem
    CONFIG ../../../../tests/test_enclave/marshal_test_enclave.config.${SGX_MODE}.xml)
endif()
