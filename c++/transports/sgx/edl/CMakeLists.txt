#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]
cmake_minimum_required(VERSION 3.24)

set(telemetry_edl_path "${CMAKE_CURRENT_SOURCE_DIR}")

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

host_edl_library(
  marshal_test_edl
  enclave_marshal_test.edl
  ${telemetry_edl_path}
  OFF
  ""
  "rpc::rpc_telemetry")

set_property(TARGET marshal_test_edl PROPERTY COMPILE_PDB_NAME marshal_test_edl)

enclave_edl_library(
  marshal_test_edl_enclave
  enclave_marshal_test.edl
  ${telemetry_edl_path}
  OFF
  ""
  "")
set_property(TARGET marshal_test_edl_enclave PROPERTY COMPILE_PDB_NAME marshal_test_edl_enclave)
