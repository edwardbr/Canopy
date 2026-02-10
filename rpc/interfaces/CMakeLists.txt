#[[
   Copyright (c) 2024 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

CanopyGenerate(
  rpc_types
  rpc/rpc_types.idl
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated
  ""
  yas_binary
  yas_compressed_binary
  yas_json
  protocol_buffers
  include_paths ${CMAKE_CURRENT_SOURCE_DIR}/.
  install_dir ${GENERATED_INSTALL_DIR}
  additional_headers stdint.h no_include_rpc_headers ON)
