#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

CanopyGenerate(
  sgx_enclave_runtime_config
  sgx_enclave_runtime/sgx_enclave_runtime_config.idl
  ${CMAKE_CURRENT_SOURCE_DIR}/interfaces
  ${CMAKE_BINARY_DIR}/generated
  ""
  yas_json
  dependencies rpc_types_idl json_idl io_uring_config_idl
  include_paths ${CMAKE_CURRENT_SOURCE_DIR}/interfaces
                ${CMAKE_CURRENT_SOURCE_DIR}/../../subcomponents/io_uring_config/interfaces
                ${CANOPY_INTERFACE_SOURCE_DIR}
  install_dir ${GENERATED_INSTALL_DIR})

message(STATUS "sgx_enclave_runtime_config configured")
