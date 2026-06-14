#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)
project(
  demo_text_tools_rpc_object
  VERSION 1.0.0
  LANGUAGES CXX)

include(CanopyCreateBlockingDll)
include(CanopyCreateSharedSchedulerDll)

CanopyGenerate(
  text_tools
  text_tools/text_tools.idl
  ${CMAKE_CURRENT_SOURCE_DIR}/idl
  ${CMAKE_BINARY_DIR}/generated
  ""
  yas_binary
  yas_json
  protocol_buffers
  dependencies rpc_types_idl
  include_paths ${CMAKE_CURRENT_SOURCE_DIR}/idl ${CANOPY_INTERFACE_SOURCE_DIR}
  install_dir ${GENERATED_INSTALL_DIR})

add_library(demo_text_tools_rpc_object STATIC src/text_tools.cpp)
target_compile_definitions(demo_text_tools_rpc_object PRIVATE ${CANOPY_DEFINES})
target_include_directories(
  demo_text_tools_rpc_object
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  PRIVATE ${CANOPY_INCLUDES})
target_link_libraries(
  demo_text_tools_rpc_object
  PUBLIC text_tools_idl
         rpc::modules
         ${CANOPY_LIBRARIES})
target_compile_options(demo_text_tools_rpc_object PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(demo_text_tools_rpc_object PRIVATE ${CANOPY_LINK_OPTIONS})
set_property(TARGET demo_text_tools_rpc_object PROPERTY COMPILE_PDB_NAME demo_text_tools_rpc_object)

if(NOT CANOPY_BUILD_COROUTINE AND TARGET transport_blocking_dll_runtime)
  CanopyCreateBlockingDll(
    demo_text_tools_blocking_dll
    SOURCES
    src/module.cpp
    LINK_LIBRARIES
    demo_text_tools_rpc_object
    text_tools_idl
    transport_local
    rpc_log)
endif()

if(CANOPY_BUILD_COROUTINE AND TARGET transport_shared_scheduler_dll_runtime)
  CanopyCreateSharedSchedulerDll(
    demo_text_tools_coroutine_shared_dll
    SOURCES
    src/module.cpp
    LINK_LIBRARIES
    demo_text_tools_rpc_object
    text_tools_idl
    transport_local
    rpc_log)
endif()

if(CANOPY_BUILD_ENCLAVE)
  set(DEMO_TEXT_TOOLS_ENCLAVE_TELEMETRY_LIBS)
  if(CANOPY_USE_TELEMETRY)
    list(APPEND DEMO_TEXT_TOOLS_ENCLAVE_TELEMETRY_LIBS rpc_telemetry_enclave)
    if(TARGET rpc_telemetry_edl_header_enclave)
      list(APPEND DEMO_TEXT_TOOLS_ENCLAVE_TELEMETRY_LIBS rpc_telemetry_edl_header_enclave)
    endif()
  endif()

  set(DEMO_TEXT_TOOLS_ENCLAVE_PROTOBUF_LIBS)
  if(TARGET canopy_nanopb_runtime_enclave)
    list(APPEND DEMO_TEXT_TOOLS_ENCLAVE_PROTOBUF_LIBS canopy_nanopb_runtime_enclave)
  endif()
  if(TARGET rpc_types_idl_enclave AND (CANOPY_BUILD_PROTOCOL_BUFFERS OR CANOPY_BUILD_NANOPB))
    list(APPEND DEMO_TEXT_TOOLS_ENCLAVE_PROTOBUF_LIBS rpc_types_idl_enclave)
  endif()

  if(CANOPY_BUILD_COROUTINE)
    add_enclave_library(
      demo_text_tools_sgx_coroutine_enclave
      SRCS src/text_tools.cpp src/module.cpp
      HEADER_ONLY_LIBS ${CANOPY_ENCLAVE_FMT_LIB} rpc::modules_enclave
      TRUSTED_LIBS
        libcoro_enclave
        rpc_enclave
        transport_sgx_coroutine_enclave
        json_enclave
        ${DEMO_TEXT_TOOLS_ENCLAVE_TELEMETRY_LIBS}
        sgx_coroutine_transport_edl_enclave
        transport_local_enclave
        text_tools_idl_enclave
        yas_common
        ${DEMO_TEXT_TOOLS_ENCLAVE_PROTOBUF_LIBS}
        ${SGX_ENCLAVE_LIBS})
    target_compile_definitions(
      demo_text_tools_sgx_coroutine_enclave PRIVATE ${CANOPY_ENCLAVE_DEFINES}
                                                    CANOPY_RPC_OBJECT_MODULE_NAME=\"demo_text_tools_sgx_coroutine_enclave\")
    target_include_directories(
      demo_text_tools_sgx_coroutine_enclave
      PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
             "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated/include>"
             "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated/src>"
      PRIVATE ${CANOPY_ENCLAVE_LIBCXX_INCLUDES})
    target_compile_options(demo_text_tools_sgx_coroutine_enclave PRIVATE ${CANOPY_ENCLAVE_COMPILE_OPTIONS}
                                                                         ${CANOPY_WARN_OK_ENCLAVE})
    target_link_options(demo_text_tools_sgx_coroutine_enclave PRIVATE ${CANOPY_ENCLAVE_LINK_OPTIONS})
    enclave_sign(
      demo_text_tools_sgx_coroutine_enclave
      KEY ../../../tests/test_enclave/marshal_test_enclave_private.pem
      CONFIG rpc_object_enclave.config.${SGX_MODE}.xml)
  else()
    add_enclave_library(
      demo_text_tools_sgx_blocking_enclave
      SRCS src/text_tools.cpp src/module.cpp
      HEADER_ONLY_LIBS ${CANOPY_ENCLAVE_FMT_LIB} rpc::modules_enclave
      TRUSTED_LIBS
        rpc_enclave
        transport_sgx_blocking_object_runtime_enclave
        sgx_blocking_transport_edl_enclave
        ${DEMO_TEXT_TOOLS_ENCLAVE_TELEMETRY_LIBS}
        json_enclave
        transport_sgx_blocking_enclave
        transport_local_enclave
        sgx_blocking_marshal_idl_enclave
        text_tools_idl_enclave
        yas_common
        ${DEMO_TEXT_TOOLS_ENCLAVE_PROTOBUF_LIBS}
        ${SGX_ENCLAVE_LIBS})
    target_compile_definitions(
      demo_text_tools_sgx_blocking_enclave PRIVATE ${CANOPY_ENCLAVE_DEFINES}
                                                   CANOPY_RPC_OBJECT_MODULE_NAME=\"demo_text_tools_sgx_blocking_enclave\")
    target_include_directories(
      demo_text_tools_sgx_blocking_enclave
      PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
             "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated/include>"
             "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated/src>"
      PRIVATE ${CANOPY_ENCLAVE_LIBCXX_INCLUDES})
    target_compile_options(demo_text_tools_sgx_blocking_enclave PRIVATE ${CANOPY_ENCLAVE_COMPILE_OPTIONS}
                                                                        ${CANOPY_WARN_OK_ENCLAVE})
    target_link_options(demo_text_tools_sgx_blocking_enclave PRIVATE ${CANOPY_ENCLAVE_LINK_OPTIONS})
    enclave_sign(
      demo_text_tools_sgx_blocking_enclave
      KEY ../../../tests/test_enclave/marshal_test_enclave_private.pem
      CONFIG rpc_object_enclave.config.${SGX_MODE}.xml)
  endif()
endif()

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(demo_text_tools_rpc_object PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()
