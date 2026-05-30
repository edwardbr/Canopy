#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]
cmake_minimum_required(VERSION 3.24)

set(telemetry_edl_path "${CMAKE_CURRENT_SOURCE_DIR}")

CanopyGenerate(
  sgx_blocking_marshal
  edl/sgx_blocking_marshal.idl
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_BINARY_DIR}/generated
  ""
  yas_binary
  dependencies rpc_types_idl json_idl
  include_paths ${CMAKE_SOURCE_DIR}/interfaces
  install_dir ${GENERATED_INSTALL_DIR})

add_untrusted_header(
  rpc_telemetry_edl_header
  EDL ${telemetry_edl_path}/rpc_telemetry.edl
  EDL_SEARCH_PATHS)

if(CANOPY_USE_TELEMETRY)
  add_trusted_header(
    rpc_telemetry_edl_header_enclave
    EDL ${telemetry_edl_path}/rpc_telemetry.edl
    EDL_SEARCH_PATHS)

endif()

set(SGX_BLOCKING_TRANSPORT_EDL_HOST_LIBS)
if(CANOPY_USE_TELEMETRY)
  list(APPEND SGX_BLOCKING_TRANSPORT_EDL_HOST_LIBS rpc::rpc_telemetry)
endif()

host_edl_library(
  sgx_blocking_transport_edl
  sgx_blocking_transport.edl
  ${telemetry_edl_path}
  OFF
  ""
  "${SGX_BLOCKING_TRANSPORT_EDL_HOST_LIBS}"
  EXTRA_DEPENDS
  ${telemetry_edl_path}/rpc_telemetry.edl)

set_property(TARGET sgx_blocking_transport_edl PROPERTY COMPILE_PDB_NAME sgx_blocking_transport_edl)

enclave_edl_library(
  sgx_blocking_transport_edl_enclave
  sgx_blocking_transport.edl
  ${telemetry_edl_path}
  OFF
  ""
  ""
  EXTRA_DEPENDS
  ${telemetry_edl_path}/rpc_telemetry.edl)
set_property(TARGET sgx_blocking_transport_edl_enclave PROPERTY COMPILE_PDB_NAME sgx_blocking_transport_edl_enclave)
