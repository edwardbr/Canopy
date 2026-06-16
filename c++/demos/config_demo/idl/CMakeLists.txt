#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

CanopyGenerate(
  config_demo
  config_demo/config_demo.idl
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated
  ""
  yas_binary
  yas_json
  dependencies calculator_idl
  include_paths ${CMAKE_CURRENT_SOURCE_DIR}/.
                ${CANOPY_CPP_SOURCE_DIR}/demos/rpc_objects/calculator/idl
                ${CANOPY_INTERFACE_SOURCE_DIR}
  install_dir ${GENERATED_INSTALL_DIR})

CanopyGenerate(
  config_demo_config
  config_demo_config/config_demo_config.idl
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated
  ""
  yas_json
  dependencies connection_factory_config_idl
  include_paths ${CMAKE_CURRENT_SOURCE_DIR}/.
                ${CANOPY_INTERFACE_SOURCE_DIR}
                ${CANOPY_CPP_SOURCE_DIR}/connection_factory/interfaces
  install_dir ${GENERATED_INSTALL_DIR})
