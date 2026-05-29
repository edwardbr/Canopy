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
                                                                     secure_coroutine_module_idl streaming_spsc_queue)

add_library(transport_sgx_coroutine_enclave_headers INTERFACE)
target_include_directories(
  transport_sgx_coroutine_enclave_headers
  INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../enclave/include>"
            "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../local/include>"
            "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../../security/attestation/include>"
            "$<INSTALL_INTERFACE:include>")
target_link_libraries(transport_sgx_coroutine_enclave_headers INTERFACE transport_secure_coroutine_module_headers)

if(CANOPY_BUILD_ENCLAVE)
  host_edl_library(
    canopy_coroutine_edl
    canopy_coroutine_enclave.edl
    "${sgx_coroutine_edl_path}"
    OFF
    ""
    "secure_coroutine_module_idl"
    EXTRA_DEPENDS
    ${sgx_coroutine_edl_path}/canopy_coroutine_enclave.edl
    EXTRA_LIBS transport_secure_coroutine_module_headers streaming_spsc_queue)

  set_property(TARGET canopy_coroutine_edl PROPERTY COMPILE_PDB_NAME canopy_coroutine_edl)

  enclave_edl_library(
    canopy_coroutine_edl_enclave
    canopy_coroutine_enclave.edl
    "${sgx_coroutine_edl_path}"
    OFF
    ""
    "secure_coroutine_module_idl_enclave"
    EXTRA_DEPENDS
    ${sgx_coroutine_edl_path}/canopy_coroutine_enclave.edl)

  set_property(TARGET canopy_coroutine_edl_enclave PROPERTY COMPILE_PDB_NAME canopy_coroutine_edl_enclave)
endif()
