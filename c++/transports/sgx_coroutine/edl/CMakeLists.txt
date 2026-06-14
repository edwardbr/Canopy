#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

set(sgx_coroutine_edl_path "${CMAKE_CURRENT_SOURCE_DIR}")

add_library(transport_sgx_coroutine_host_headers INTERFACE)
target_include_directories(
  transport_sgx_coroutine_host_headers INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../host/include>"
                                                 "$<INSTALL_INTERFACE:include>")
target_link_libraries(transport_sgx_coroutine_host_headers INTERFACE transport_secure_coroutine_module_headers
                                                                     secure_coroutine_module_idl stream_layers_idl
                                                                     streaming_spsc_queue)

add_library(transport_sgx_coroutine_enclave_headers INTERFACE)
target_include_directories(
  transport_sgx_coroutine_enclave_headers
  INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../enclave/include>"
            "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../local/include>"
            "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../../security/attestation/include>"
            "$<INSTALL_INTERFACE:include>")
target_link_libraries(transport_sgx_coroutine_enclave_headers INTERFACE transport_secure_coroutine_module_headers
                                                                       rpc::modules_enclave)

if(CANOPY_BUILD_ENCLAVE)
  host_edl_library(
    sgx_coroutine_transport_edl
    sgx_coroutine_transport.edl
    "${sgx_coroutine_edl_path}"
    OFF
    ""
    "secure_coroutine_module_idl"
    EXTRA_DEPENDS
    ${sgx_coroutine_edl_path}/sgx_coroutine_transport.edl
    EXTRA_LIBS transport_secure_coroutine_module_headers streaming_spsc_queue)

  set_property(TARGET sgx_coroutine_transport_edl PROPERTY COMPILE_PDB_NAME sgx_coroutine_transport_edl)

  enclave_edl_library(
    sgx_coroutine_transport_edl_enclave
    sgx_coroutine_transport.edl
    "${sgx_coroutine_edl_path}"
    OFF
    ""
    "secure_coroutine_module_idl_enclave"
    EXTRA_DEPENDS
    ${sgx_coroutine_edl_path}/sgx_coroutine_transport.edl)

  set_property(TARGET sgx_coroutine_transport_edl_enclave PROPERTY COMPILE_PDB_NAME sgx_coroutine_transport_edl_enclave)
endif()
