#[[
   Copyright (c) 2024 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

CanopyGenerate(
  example_shared
  example_shared/example_shared.idl
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated
  ""
  yas_binary
  yas_compressed_binary
  yas_json
  protocol_buffers
  mock example_shared/example_shared_mock.h
  include_paths ${CMAKE_CURRENT_SOURCE_DIR}/.
  install_dir ${GENERATED_INSTALL_DIR})

CanopyGenerate(
  example_import
  example_import/example_import.idl
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated
  ""
  yas_binary
  yas_compressed_binary
  yas_json
  protocol_buffers
  mock example_import/example_import_mock.h
  dependencies example_shared_idl
  include_paths ${CMAKE_CURRENT_SOURCE_DIR}/.
  install_dir ${GENERATED_INSTALL_DIR})

CanopyGenerate(
  example
  example/example.idl
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated
  ""
  yas_binary
  yas_compressed_binary
  yas_json
  protocol_buffers
  mock example/example_mock.h
  dependencies example_import_idl example_shared_idl
  include_paths ${CMAKE_CURRENT_SOURCE_DIR}/.
  install_dir ${GENERATED_INSTALL_DIR})

CanopyGenerate(
  fuzz_test
  fuzz_test/fuzz_test.idl
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated
  ""
  yas_binary
  yas_compressed_binary
  yas_json
  protocol_buffers
  mock fuzz_test/fuzz_test_mock.h
  include_paths ${CMAKE_CURRENT_SOURCE_DIR}/.)
