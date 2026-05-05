#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]
cmake_minimum_required(VERSION 3.24)

message("marshal_test_enclave")

# ######################################################################################################################
#marshal_test_enclave

if(${CANOPY_BUILD_ENCLAVE} AND NOT ${CANOPY_BUILD_COROUTINE})
  set(MARSHAL_TEST_TELEMETRY_ENCLAVE_LIBS)
  if(CANOPY_USE_TELEMETRY)
    list(APPEND MARSHAL_TEST_TELEMETRY_ENCLAVE_LIBS rpc_telemetry_enclave rpc_telemetry_edl_header_enclave)
  endif()

  set(MARSHAL_TEST_PROTOBUF_ENCLAVE_LIBS)
  if(TARGET canopy_nanopb_runtime_enclave)
    list(APPEND MARSHAL_TEST_PROTOBUF_ENCLAVE_LIBS canopy_nanopb_runtime_enclave)
  endif()
  if(TARGET rpc_types_idl_enclave AND (CANOPY_BUILD_PROTOCOL_BUFFERS OR CANOPY_BUILD_NANOPB))
    list(APPEND MARSHAL_TEST_PROTOBUF_ENCLAVE_LIBS rpc_types_idl_enclave)
  endif()

  add_enclave_library(
    marshal_test_enclave
    SRCS marshal_test_enclave.cpp
    HEADER_ONLY_LIBS
      ${CANOPY_ENCLAVE_FMT_LIB}
    TRUSTED_LIBS
      rpc_enclave
      marshal_test_edl_enclave
      ${MARSHAL_TEST_TELEMETRY_ENCLAVE_LIBS}
      transport_sgx_enclave
      transport_local_enclave
      common_enclave
      io_uring_idl_enclave
      enclave_marshal_test_idl_enclave
      example_import_idl_enclave
      example_idl_enclave
      example_shared_idl_enclave
      yas_common
      ${MARSHAL_TEST_PROTOBUF_ENCLAVE_LIBS}
      ${SGX_ENCLAVE_LIBS})

  target_compile_definitions(marshal_test_enclave PRIVATE ${CANOPY_ENCLAVE_DEFINES})
  target_include_directories(
    marshal_test_enclave
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
           "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../common/include>"
           "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../subcomponents/io_uring/include>"
           "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../transports/local/include>"
           "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../transports/sgx/include>"
           "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated/include>"
           "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated/src>"
           "$<INSTALL_INTERFACE:include/marshalled_tests>"
    PRIVATE ${CANOPY_ENCLAVE_LIBCXX_INCLUDES})

  if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(EXTRA_MARSHAL_TEST_ENCLAVE_COMPILE_OPTIONS -Wno-unused-parameter)
  endif()

  target_compile_options(marshal_test_enclave PRIVATE ${CANOPY_ENCLAVE_COMPILE_OPTIONS} ${CANOPY_WARN_OK_ENCLAVE}
                                                      ${EXTRA_MARSHAL_TEST_ENCLAVE_COMPILE_OPTIONS})
  target_link_options(marshal_test_enclave PRIVATE ${CANOPY_ENCLAVE_LINK_OPTIONS})

  set_property(TARGET marshal_test_enclave PROPERTY COMPILE_PDB_NAME marshal_test_enclave)
  if(CANOPY_ENABLE_CLANG_TIDY)
    set_target_properties(marshal_test_enclave PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
  endif()
  enclave_sign(
    marshal_test_enclave
    KEY marshal_test_enclave_private.pem
    CONFIG marshal_test_enclave.config.${SGX_MODE}.xml)
endif()

if(${CANOPY_BUILD_ENCLAVE} AND ${CANOPY_BUILD_COROUTINE})
  set(MARSHAL_TEST_COROUTINE_TELEMETRY_ENCLAVE_LIBS)
  if(CANOPY_USE_TELEMETRY)
    list(APPEND MARSHAL_TEST_COROUTINE_TELEMETRY_ENCLAVE_LIBS rpc_telemetry_enclave)
  endif()

  set(MARSHAL_TEST_COROUTINE_PROTOBUF_ENCLAVE_LIBS)
  if(TARGET canopy_nanopb_runtime_enclave)
    list(APPEND MARSHAL_TEST_COROUTINE_PROTOBUF_ENCLAVE_LIBS canopy_nanopb_runtime_enclave)
  endif()
  if(TARGET rpc_types_idl_enclave AND (CANOPY_BUILD_PROTOCOL_BUFFERS OR CANOPY_BUILD_NANOPB))
    list(APPEND MARSHAL_TEST_COROUTINE_PROTOBUF_ENCLAVE_LIBS rpc_types_idl_enclave)
  endif()

  add_enclave_library(
    marshal_test_coroutine_enclave
    SRCS marshal_test_coroutine_enclave.cpp
    HEADER_ONLY_LIBS
      ${CANOPY_ENCLAVE_FMT_LIB}
    TRUSTED_LIBS
      libcoro_enclave
      rpc_enclave
      streaming_enclave
      transport_streaming_enclave
      streaming_transport_idl_enclave
      transport_sgx_coroutine_enclave
      ${MARSHAL_TEST_COROUTINE_TELEMETRY_ENCLAVE_LIBS}
      streaming_spsc_queue_enclave
      canopy_coroutine_edl_enclave
      transport_local_enclave
      common_enclave
      io_uring_idl_enclave
      example_import_idl_enclave
      example_idl_enclave
      example_shared_idl_enclave
      yas_common
      ${MARSHAL_TEST_COROUTINE_PROTOBUF_ENCLAVE_LIBS}
      ${SGX_ENCLAVE_LIBS})

  target_compile_definitions(marshal_test_coroutine_enclave PRIVATE ${CANOPY_ENCLAVE_DEFINES})
  target_include_directories(
    marshal_test_coroutine_enclave
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
           "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../common/include>"
           "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../subcomponents/io_uring/include>"
           "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../transports/local/include>"
           "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../transports/sgx_coroutine/include>"
           "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated/include>"
           "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated/src>"
           "$<INSTALL_INTERFACE:include/marshalled_tests>"
    PRIVATE ${CANOPY_ENCLAVE_LIBCXX_INCLUDES})

  if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(EXTRA_MARSHAL_TEST_COROUTINE_ENCLAVE_COMPILE_OPTIONS -Wno-unused-parameter)
  endif()

  target_compile_options(
    marshal_test_coroutine_enclave PRIVATE ${CANOPY_ENCLAVE_COMPILE_OPTIONS} ${CANOPY_WARN_OK_ENCLAVE}
                                           ${EXTRA_MARSHAL_TEST_COROUTINE_ENCLAVE_COMPILE_OPTIONS})
  target_link_options(marshal_test_coroutine_enclave PRIVATE ${CANOPY_ENCLAVE_LINK_OPTIONS})

  set_property(TARGET marshal_test_coroutine_enclave PROPERTY COMPILE_PDB_NAME marshal_test_coroutine_enclave)
  if(CANOPY_ENABLE_CLANG_TIDY)
    set_target_properties(marshal_test_coroutine_enclave PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
  endif()

  enclave_sign(
    marshal_test_coroutine_enclave
    KEY marshal_test_enclave_private.pem
    CONFIG marshal_test_enclave.config.${SGX_MODE}.xml)
endif()
