#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

# WebSocket transport protocol is host-only and requires coroutines
if(CANOPY_BUILD_COROUTINE)

  CanopyGenerate(
    websocket_protocol
    websocket_protocol/websocket_protocol.idl
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
