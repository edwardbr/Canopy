#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

# TCP transport is host-only and requires coroutines
if(CANOPY_BUILD_COROUTINE)

  # idl
  CanopyGenerate(
    tcp
    tcp/tcp.idl
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_BINARY_DIR}/generated
    ""
    yas_binary
    yas_compressed_binary
    yas_json
    include_paths ${CMAKE_CURRENT_SOURCE_DIR}/.
    dependencies rpc_types_idl
    install_dir ${GENERATED_INSTALL_DIR})

endif()
