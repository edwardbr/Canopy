#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

CanopyGenerate(
  secure_coroutine_module
  secure_coroutine_module/secure_coroutine_module.idl
  ${CMAKE_CURRENT_SOURCE_DIR}/interfaces
  ${CMAKE_BINARY_DIR}/generated
  ""
  yas_binary
  yas_json
  dependencies rpc_types_idl noop_idl streaming_transport_idl json_idl
  include_paths ${CMAKE_SOURCE_DIR}/interfaces
  install_dir ${GENERATED_INSTALL_DIR})

add_library(transport_secure_coroutine_module_headers INTERFACE)
target_include_directories(
  transport_secure_coroutine_module_headers INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
                                                      "$<INSTALL_INTERFACE:include>")
