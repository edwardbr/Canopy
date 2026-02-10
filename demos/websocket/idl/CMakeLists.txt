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
  dependencies rpc_types_idl
  install_dir ${GENERATED_INSTALL_DIR}
  include_paths ${CMAKE_CURRENT_SOURCE_DIR}/.)

# Generate JavaScript protobuf files for WebSocket demo
find_program(NPM_EXECUTABLE npm)
find_program(NODE_EXECUTABLE node)

if(NPM_EXECUTABLE AND NODE_EXECUTABLE)
  set(CLIENT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../client")
  set(SERVER_WWW_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../server/www")
  set(PROTO_SRC_DIR "${CMAKE_BINARY_DIR}/generated/src")
  set(PBJS_EXECUTABLE "${CLIENT_DIR}/node_modules/.bin/pbjs")

  # Ensure npm dependencies are installed
  add_custom_command(
    OUTPUT "${PBJS_EXECUTABLE}"
    COMMAND ${NPM_EXECUTABLE} install --save-dev protobufjs-cli
    WORKING_DIRECTORY "${CLIENT_DIR}"
    COMMENT "Installing protobufjs-cli via npm")

  # Generate browser version (static module)
  add_custom_command(
    OUTPUT "${SERVER_WWW_DIR}/websocket_proto.js"
    COMMAND ${PBJS_EXECUTABLE} -t static-module --path "${PROTO_SRC_DIR}" -o "${SERVER_WWW_DIR}/websocket_proto.js"
            websocket_demo/protobuf/websocket_demo_all.proto
    DEPENDS "${PBJS_EXECUTABLE}"
            "${PROTO_SRC_DIR}/websocket_demo/protobuf/websocket_demo_all.proto"
            "${PROTO_SRC_DIR}/websocket_demo/protobuf/schema/websocket_demo.proto"
            "${PROTO_SRC_DIR}/websocket_demo/protobuf/schema/websocket_demo_v1.proto"
            websocket_demo_idl
    WORKING_DIRECTORY "${CLIENT_DIR}"
    COMMENT "Generating browser protobuf JavaScript for WebSocket demo")

  # Generate Node.js version (CommonJS)
  add_custom_command(
    OUTPUT "${CLIENT_DIR}/websocket_proto.js"
    COMMAND ${PBJS_EXECUTABLE} -t static-module -w commonjs --path "${PROTO_SRC_DIR}" -o
            "${CLIENT_DIR}/websocket_proto.js" websocket_demo/protobuf/websocket_demo_all.proto
    DEPENDS "${PBJS_EXECUTABLE}"
            "${PROTO_SRC_DIR}/websocket_demo/protobuf/websocket_demo_all.proto"
            "${PROTO_SRC_DIR}/websocket_demo/protobuf/schema/websocket_demo.proto"
            "${PROTO_SRC_DIR}/websocket_demo/protobuf/schema/websocket_demo_v1.proto"
            websocket_demo_idl
    WORKING_DIRECTORY "${CLIENT_DIR}"
    COMMENT "Generating Node.js protobuf JavaScript for WebSocket demo")

  # Create custom target that depends on both generated files
  add_custom_target(websocket_proto_js ALL DEPENDS "${SERVER_WWW_DIR}/websocket_proto.js"
                                                   "${CLIENT_DIR}/websocket_proto.js")

  # Make sure proto generation happens before JavaScript generation
  add_dependencies(websocket_proto_js websocket_demo_idl)

  message(STATUS "WebSocket protobuf JavaScript generation enabled")
else()
  if(NOT NPM_EXECUTABLE)
    message(WARNING "npm not found - WebSocket JavaScript protobuf files will not be generated automatically")
  endif()
  if(NOT NODE_EXECUTABLE)
    message(WARNING "node not found - WebSocket JavaScript protobuf files will not be generated automatically")
  endif()
endif()
