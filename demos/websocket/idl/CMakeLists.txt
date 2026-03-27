#[[
   Copyright (c) 2024 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

CanopyGenerate(
  websocket_demo
  websocket_demo/websocket_demo.idl
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

include(CanopyJavascriptGenerate)

set(_client_dir   "${CMAKE_CURRENT_SOURCE_DIR}/../client")
set(_www_dir      "${CMAKE_CURRENT_SOURCE_DIR}/../server/www")
set(_transport_js "${CMAKE_CURRENT_SOURCE_DIR}/../../../transports/websocket/js/canopy_websocket_transport.js")

# Positional args (idl, base_dir) kept separate from keyword args so that output_dir
# remains the 4th positional parameter when the two sets are combined in each call.
set(_js_idl    websocket_demo/websocket_demo.idl)
set(_js_base   ${CMAKE_CURRENT_SOURCE_DIR})
set(_js_kwargs
  dependencies rpc_types_idl websocket_protocol_idl
  include_paths ${CMAKE_CURRENT_SOURCE_DIR}/.
  copy_files "${_transport_js}"
  compile_proto_js)

CanopyJavascriptGenerate(websocket_demo_client
  ${_js_idl} ${_js_base} "${_client_dir}/generated"
  ${_js_kwargs} proto_js_format commonjs)

CanopyJavascriptGenerate(websocket_demo_www
  ${_js_idl} ${_js_base} "${_www_dir}/generated"
  ${_js_kwargs})
