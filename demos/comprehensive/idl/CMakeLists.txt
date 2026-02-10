#[[
   Copyright (c) 2024 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

# Generate code from IDL Note: protocol_buffers is disabled because rpc::optimistic_ptr has no protobuf equivalent
CanopyGenerate(
  comprehensive
  comprehensive/comprehensive.idl
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated
  ""
  yas_binary
  yas_compressed_binary
  yas_json
  protocol_buffers
  mock comprehensive/comprehensive_mock.h
  include_paths ${CMAKE_CURRENT_SOURCE_DIR}/.
  install_dir ${GENERATED_INSTALL_DIR})

# Suppress generator bug warning (misleading indentation in generated code)
target_compile_options(comprehensive_idl PRIVATE -Wno-misleading-indentation)
