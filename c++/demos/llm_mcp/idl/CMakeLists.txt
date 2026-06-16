#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

CanopyGenerate(
  llm_mcp
  llm_mcp/llm_mcp.idl
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated
  ""
  yas_json
  dependencies rpc_types_idl
  include_paths ${CMAKE_CURRENT_SOURCE_DIR}/. ${CANOPY_INTERFACE_SOURCE_DIR}
  install_dir ${GENERATED_INSTALL_DIR})

CanopyGenerate(
  llm_mcp_harness
  llm_mcp_harness/llm_mcp_harness.idl
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated
  ""
  yas_json
  dependencies rpc_types_idl calculator_idl text_tools_idl
  include_paths ${CMAKE_CURRENT_SOURCE_DIR}/.
                ${CANOPY_CPP_SOURCE_DIR}/demos/rpc_objects/calculator/idl
                ${CANOPY_CPP_SOURCE_DIR}/demos/rpc_objects/text_tools/idl
                ${CANOPY_INTERFACE_SOURCE_DIR}
  install_dir ${GENERATED_INSTALL_DIR})

target_compile_options(llm_mcp_idl PRIVATE -Wno-misleading-indentation)
target_compile_options(llm_mcp_harness_idl PRIVATE -Wno-misleading-indentation)
