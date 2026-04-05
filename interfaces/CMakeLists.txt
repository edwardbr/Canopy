#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
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

if(CANOPY_BUILD_COROUTINE)
  CanopyGenerate(
    streaming_transport
    streaming/stream_transport.idl
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_BINARY_DIR}/generated
    ""
    yas_binary
    yas_compressed_binary
    yas_json
    include_paths ${CMAKE_CURRENT_SOURCE_DIR}/.
    dependencies rpc_types_idl
    install_dir ${GENERATED_INSTALL_DIR})

  CanopyGenerate(
    websocket_protocol
    websocket/websocket_protocol.idl
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_BINARY_DIR}/generated
    ""
    yas_binary
    yas_compressed_binary
    yas_json
    protocol_buffers
    dependencies rpc_types_idl
    install_dir ${GENERATED_INSTALL_DIR}
    include_paths ${CMAKE_CURRENT_SOURCE_DIR}/.)
endif()
