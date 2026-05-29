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
  additional_headers stdint.h no_include_rpc_headers ON)

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

foreach(json_idl_target json_idl json_idl_enclave)
  if(TARGET ${json_idl_target})
    target_include_directories(${json_idl_target}
                               PUBLIC "$<BUILD_INTERFACE:${CANOPY_CPP_SOURCE_DIR}/subcomponents/json/include>")
  endif()
endforeach()
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

if(CANOPY_ENABLE_DEVELOPMENT_ATTESTATION_BACKENDS OR CANOPY_BUILD_SGX_ATTESTATION_TESTS)
  CanopyGenerate(
    sgx_sim_protocol
    attestation/sgx_sim_protocol.idl
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_BINARY_DIR}/generated
    ""
    canonical_crypto
    dependencies route_attestation_protocol_idl
    include_paths ${CMAKE_CURRENT_SOURCE_DIR}/.
    install_dir ${GENERATED_INSTALL_DIR})
endif()

if(CANOPY_ATTESTATION_BACKEND STREQUAL "SGX_EPID" OR CANOPY_SECURITY_ATTESTATION_BUILD_BACKEND_UNIT_TESTS)
  CanopyGenerate(
    sgx_epid_protocol
    attestation/sgx_epid_protocol.idl
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_BINARY_DIR}/generated
    ""
    canonical_crypto
    dependencies route_attestation_protocol_idl
    include_paths ${CMAKE_CURRENT_SOURCE_DIR}/.
    install_dir ${GENERATED_INSTALL_DIR})
endif()

if(CANOPY_ATTESTATION_BACKEND STREQUAL "DCAP" OR CANOPY_SECURITY_ATTESTATION_BUILD_BACKEND_UNIT_TESTS)
  CanopyGenerate(
    sgx_dcap_protocol
    attestation/sgx_dcap_protocol.idl
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_BINARY_DIR}/generated
    ""
    canonical_crypto
    dependencies route_attestation_protocol_idl
    include_paths ${CMAKE_CURRENT_SOURCE_DIR}/.
    install_dir ${GENERATED_INSTALL_DIR})
endif()

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
  install_dir ${GENERATED_INSTALL_DIR} no_include_rpc_headers ON)

CanopyGenerate(
  telemetry_types
  rpc/telemetry_types.idl
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated
  ""
  yas_binary
  dependencies rpc_types_idl logging_idl
  include_paths ${CMAKE_CURRENT_SOURCE_DIR}/.
  install_dir ${GENERATED_INSTALL_DIR} no_include_rpc_headers ON)

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
