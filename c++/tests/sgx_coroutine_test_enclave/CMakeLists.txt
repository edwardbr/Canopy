#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]
cmake_minimum_required(VERSION 3.24)

if(CANOPY_BUILD_ENCLAVE AND CANOPY_BUILD_COROUTINE)
  set(SGX_COROUTINE_TEST_TELEMETRY_ENCLAVE_LIBS)
  if(CANOPY_USE_TELEMETRY)
    list(APPEND SGX_COROUTINE_TEST_TELEMETRY_ENCLAVE_LIBS rpc_telemetry_enclave)
  endif()

  set(SGX_COROUTINE_TEST_PROTOBUF_ENCLAVE_LIBS)
  if(TARGET canopy_nanopb_runtime_enclave)
    list(APPEND SGX_COROUTINE_TEST_PROTOBUF_ENCLAVE_LIBS canopy_nanopb_runtime_enclave)
  endif()
  if(TARGET rpc_types_idl_enclave AND (CANOPY_BUILD_PROTOCOL_BUFFERS OR CANOPY_BUILD_NANOPB))
    list(APPEND SGX_COROUTINE_TEST_PROTOBUF_ENCLAVE_LIBS rpc_types_idl_enclave)
  endif()

  add_enclave_library(
    sgx_coroutine_test_enclave
    SRCS end_2_end_setup.cpp
         end_2_end_setup.h
         sgx_coroutine_test_enclave.cpp
         test_uring.cpp
         test_uring_peer2peer.cpp
         test_uring.h
    HEADER_ONLY_LIBS ${CANOPY_ENCLAVE_FMT_LIB}
    TRUSTED_LIBS
      libcoro_enclave
      rpc_enclave
      streaming_enclave
      transport_streaming_enclave
      streaming_transport_idl_enclave
      transport_sgx_coroutine_enclave
      ${SGX_COROUTINE_TEST_TELEMETRY_ENCLAVE_LIBS}
      streaming_spsc_queue_enclave
      streaming_io_uring_new_enclave
      canopy_coroutine_edl_enclave
      transport_local_enclave
      canopy_io_uring_enclave
      io_uring_test_idl_enclave
      example_idl_enclave
      yas_common
      ${SGX_COROUTINE_TEST_PROTOBUF_ENCLAVE_LIBS}
      ${SGX_ENCLAVE_LIBS})

  target_compile_definitions(sgx_coroutine_test_enclave PRIVATE ${CANOPY_ENCLAVE_DEFINES})
  target_include_directories(
    sgx_coroutine_test_enclave
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../common/include>"
           "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../transports/local/include>"
           "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated/include>"
           "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated/src>"
           "$<INSTALL_INTERFACE:include/marshalled_tests>"
    PRIVATE ${CANOPY_ENCLAVE_LIBCXX_INCLUDES})

  if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(EXTRA_SGX_COROUTINE_TEST_ENCLAVE_COMPILE_OPTIONS -Wno-unused-parameter)
  endif()

  target_compile_options(sgx_coroutine_test_enclave PRIVATE ${CANOPY_ENCLAVE_COMPILE_OPTIONS} ${CANOPY_WARN_OK_ENCLAVE}
                                                            ${EXTRA_SGX_COROUTINE_TEST_ENCLAVE_COMPILE_OPTIONS})
  target_link_options(sgx_coroutine_test_enclave PRIVATE ${CANOPY_ENCLAVE_LINK_OPTIONS})

  set_property(TARGET sgx_coroutine_test_enclave PROPERTY COMPILE_PDB_NAME sgx_coroutine_test_enclave)
  if(CANOPY_ENABLE_CLANG_TIDY)
    set_target_properties(sgx_coroutine_test_enclave PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
  endif()

  enclave_sign(
    sgx_coroutine_test_enclave
    KEY ../test_enclave/marshal_test_enclave_private.pem
    CONFIG ../test_enclave/marshal_test_enclave.config.${SGX_MODE}.xml)
endif()
