#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

include(CanopyJavascriptGenerate)

set(transport_js "${CANOPY_CPP_SOURCE_DIR}/transports/websocket/js/canopy_websocket_transport.js")
set(demo_idl websocket_demo/websocket_demo.idl)

CanopyGenerate(
  websocket_demo
  ${demo_idl}
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated
  ""
  yas_binary
  yas_compressed_binary
  yas_json
  protocol_buffers
  dependencies rpc_types_idl websocket_protocol_idl
  install_dir ${GENERATED_INSTALL_DIR}
  include_paths ${CMAKE_CURRENT_SOURCE_DIR}/.)

canopyjavascriptgenerate(
  websocket_demo_client
  ${demo_idl}
  ${CMAKE_CURRENT_SOURCE_DIR}
  "${CMAKE_CURRENT_SOURCE_DIR}/../client/generated"
  dependencies
  rpc_types_idl
  websocket_protocol_idl
  include_paths
  ${CMAKE_CURRENT_SOURCE_DIR}/.
  copy_files
  "${transport_js}"
  compile_proto_js
  proto_js_format
  commonjs)

canopyjavascriptgenerate(
  websocket_demo_www
  ${demo_idl}
  ${CMAKE_CURRENT_SOURCE_DIR}
  "${CMAKE_CURRENT_SOURCE_DIR}/../server/www/generated"
  dependencies
  rpc_types_idl
  websocket_protocol_idl
  include_paths
  ${CMAKE_CURRENT_SOURCE_DIR}/.
  copy_files
  "${transport_js}"
  compile_proto_js)
