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
  canonical_crypto
  include_paths ${CMAKE_CURRENT_SOURCE_DIR}/.
  install_dir ${GENERATED_INSTALL_DIR}
  additional_headers stdint.h no_include_rpc_headers)

CanopyGenerate(
  json
  json/json.idl
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated
  ""
  yas_binary
  yas_compressed_binary
  yas_json
  protocol_buffers
  nanopb
  include_paths ${CMAKE_CURRENT_SOURCE_DIR}/.
  install_dir ${GENERATED_INSTALL_DIR})

target_include_directories(json_idl PUBLIC "$<BUILD_INTERFACE:${CANOPY_CPP_SOURCE_DIR}/subcomponents/json/include>")
foreach(json_generated_target json_protobuf_generated json_nanopb_generated)
  if(TARGET ${json_generated_target})
    target_include_directories(${json_generated_target}
                               PRIVATE "$<BUILD_INTERFACE:${CANOPY_CPP_SOURCE_DIR}/subcomponents/json/include>")
  endif()
endforeach()

CanopyGenerate(
  route_attestation_protocol
  attestation/route_attestation_protocol.idl
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated
  ""
  yas_binary
  yas_compressed_binary
  yas_json
  protocol_buffers
  canonical_crypto
  dependencies rpc_types_idl
  include_paths ${CMAKE_CURRENT_SOURCE_DIR}/.
  install_dir ${GENERATED_INSTALL_DIR})

CanopyGenerate(
  protected_rpc_protocol
  attestation/protected_rpc_protocol.idl
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated
  ""
  canonical_crypto
  dependencies rpc_types_idl
  include_paths ${CMAKE_CURRENT_SOURCE_DIR}/.
  install_dir ${GENERATED_INSTALL_DIR})

CanopyGenerate(
  noop
  rpc/noop.idl
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated
  ""
  yas_binary
  dependencies rpc_types_idl
  include_paths ${CMAKE_CURRENT_SOURCE_DIR}/.
  install_dir ${GENERATED_INSTALL_DIR})

CanopyGenerate(
  logging
  rpc/logging.idl
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated
  ""
  yas_binary
  dependencies rpc_types_idl
  include_paths ${CMAKE_CURRENT_SOURCE_DIR}/.
  install_dir ${GENERATED_INSTALL_DIR} no_include_rpc_headers)

CanopyGenerate(
  telemetry_types
  rpc/telemetry_types.idl
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated
  ""
  yas_binary
  dependencies rpc_types_idl logging_idl
  include_paths ${CMAKE_CURRENT_SOURCE_DIR}/.
  install_dir ${GENERATED_INSTALL_DIR} no_include_rpc_headers)

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
  stream_layers
  streaming/stream_layers.idl
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated
  ""
  yas_binary
  yas_json
  include_paths ${CMAKE_CURRENT_SOURCE_DIR}/.
  dependencies rpc_types_idl json_idl
  install_dir ${GENERATED_INSTALL_DIR})

if(CANOPY_BUILD_WEBSOCKET)
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
