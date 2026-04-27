#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

set(sgx_coroutine_edl_path "${CMAKE_CURRENT_SOURCE_DIR}")

CanopyGenerate(
  canopy_coroutine_enclave
  edl/canopy_coroutine_enclave.idl
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_BINARY_DIR}/generated
  ""
  yas_binary
  dependencies rpc_types_idl
  include_paths ${CMAKE_SOURCE_DIR}/interfaces
  install_dir ${GENERATED_INSTALL_DIR})

host_edl_library(
  canopy_coroutine_edl
  canopy_coroutine_enclave.edl
  "${sgx_coroutine_edl_path}"
  OFF
  ""
  ""
  EXTRA_DEPENDS
  ${sgx_coroutine_edl_path}/canopy_coroutine_enclave.edl)

set_property(TARGET canopy_coroutine_edl PROPERTY COMPILE_PDB_NAME canopy_coroutine_edl)

enclave_edl_library(
  canopy_coroutine_edl_enclave
  canopy_coroutine_enclave.edl
  "${sgx_coroutine_edl_path}"
  OFF
  ""
  ""
  EXTRA_DEPENDS
  ${sgx_coroutine_edl_path}/canopy_coroutine_enclave.edl)

set_property(TARGET canopy_coroutine_edl_enclave PROPERTY COMPILE_PDB_NAME canopy_coroutine_edl_enclave)
