#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

set(transport_js "${CANOPY_CPP_SOURCE_DIR}/transports/untrusted_web/js/untrusted_web_transport.js")
set(demo_idl websocket_demo/websocket_demo.idl)

set(CANOPY_WEBSOCKET_DEMO_ENCODING
    "AUTO"
    CACHE STRING "Encoding generated and used by the websocket demo")
set_property(CACHE CANOPY_WEBSOCKET_DEMO_ENCODING PROPERTY STRINGS "AUTO" "NANOPB" "PROTOCOL_BUFFERS")

if(NOT CANOPY_WEBSOCKET_DEMO_ENCODING MATCHES "^(AUTO|NANOPB|PROTOCOL_BUFFERS)$")
  message(WARNING "Invalid CANOPY_WEBSOCKET_DEMO_ENCODING '${CANOPY_WEBSOCKET_DEMO_ENCODING}', defaulting to AUTO")
  set(CANOPY_WEBSOCKET_DEMO_ENCODING
      "AUTO"
      CACHE STRING "Encoding generated and used by the websocket demo" FORCE)
endif()

if(CANOPY_WEBSOCKET_DEMO_ENCODING STREQUAL "AUTO")
  if(CANOPY_BUILD_NANOPB)
    set(CANOPY_WEBSOCKET_DEMO_ENCODING "NANOPB")
  elseif(CANOPY_BUILD_PROTOCOL_BUFFERS)
    set(CANOPY_WEBSOCKET_DEMO_ENCODING "PROTOCOL_BUFFERS")
  else()
    message(FATAL_ERROR "The websocket demo requires CANOPY_BUILD_NANOPB or CANOPY_BUILD_PROTOCOL_BUFFERS")
  endif()
endif()

set(websocket_demo_serialisers yas_binary yas_json)

if(CANOPY_WEBSOCKET_DEMO_ENCODING STREQUAL "NANOPB")
  if(NOT CANOPY_BUILD_NANOPB)
    message(FATAL_ERROR "CANOPY_WEBSOCKET_DEMO_ENCODING=NANOPB requires CANOPY_BUILD_NANOPB=ON")
  endif()
  list(APPEND websocket_demo_serialisers nanopb)
  set(CANOPY_WEBSOCKET_DEMO_ENCODING_VALUE
      "rpc::encoding::nanopb"
      CACHE INTERNAL "C++ encoding value used by the websocket demo" FORCE)
  set(CANOPY_WEBSOCKET_DEMO_ENCODING_JS
      32
      CACHE INTERNAL "JavaScript encoding value used by the websocket demo" FORCE)
elseif(CANOPY_WEBSOCKET_DEMO_ENCODING STREQUAL "PROTOCOL_BUFFERS")
  if(NOT CANOPY_BUILD_PROTOCOL_BUFFERS)
    message(FATAL_ERROR "CANOPY_WEBSOCKET_DEMO_ENCODING=PROTOCOL_BUFFERS requires CANOPY_BUILD_PROTOCOL_BUFFERS=ON")
  endif()
  list(APPEND websocket_demo_serialisers protocol_buffers)
  set(CANOPY_WEBSOCKET_DEMO_ENCODING_VALUE
      "rpc::encoding::protocol_buffers"
      CACHE INTERNAL "C++ encoding value used by the websocket demo" FORCE)
  set(CANOPY_WEBSOCKET_DEMO_ENCODING_JS
      16
      CACHE INTERNAL "JavaScript encoding value used by the websocket demo" FORCE)
endif()

message("CANOPY_WEBSOCKET_DEMO_ENCODING ${CANOPY_WEBSOCKET_DEMO_ENCODING} -> ${CANOPY_WEBSOCKET_DEMO_ENCODING_VALUE}")

include(CanopyJavascriptGenerate)

if(CANOPY_WEBSOCKET_DEMO_CALCULATOR_ONLY)
  set(CANOPY_WEBSOCKET_DEMO_CALCULATOR_ONLY_JS true)
else()
  set(CANOPY_WEBSOCKET_DEMO_CALCULATOR_ONLY_JS false)
endif()

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/websocket_demo_config.js.in"
               "${CMAKE_CURRENT_SOURCE_DIR}/../server/www/generated/websocket_demo_config.js" @ONLY)

CanopyGenerate(
  websocket_demo
  ${demo_idl}
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated
  ""
  ${websocket_demo_serialisers}
  dependencies rpc_types_idl websocket_protocol_idl network_config_idl
  install_dir ${GENERATED_INSTALL_DIR}
  include_paths
  ${CMAKE_CURRENT_SOURCE_DIR}/.
  ${CANOPY_INTERFACE_SOURCE_DIR}
  ${CANOPY_CPP_SOURCE_DIR}/subcomponents/network_config/interfaces)

canopyjavascriptgenerate(
  websocket_demo_client
  ${demo_idl}
  ${CMAKE_CURRENT_SOURCE_DIR}
  "${CMAKE_CURRENT_SOURCE_DIR}/../client/generated"
  dependencies
  rpc_types_idl
  websocket_protocol_idl
  network_config_idl
  include_paths
  ${CMAKE_CURRENT_SOURCE_DIR}/.
  ${CANOPY_INTERFACE_SOURCE_DIR}
  ${CANOPY_CPP_SOURCE_DIR}/subcomponents/network_config/interfaces
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
  network_config_idl
  include_paths
  ${CMAKE_CURRENT_SOURCE_DIR}/.
  ${CANOPY_INTERFACE_SOURCE_DIR}
  ${CANOPY_CPP_SOURCE_DIR}/subcomponents/network_config/interfaces
  copy_files
  "${transport_js}"
  compile_proto_js)
