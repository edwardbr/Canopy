#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
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
  canonical_crypto
  mock example_shared/example_shared_mock.h
  dependencies json_idl
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
  canonical_crypto
  mock example_import/example_import_mock.h
  dependencies example_shared_idl json_idl
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
  canonical_crypto
  mock example/example_mock.h
  dependencies example_import_idl example_shared_idl json_idl
  include_paths ${CMAKE_CURRENT_SOURCE_DIR}/.
  install_dir ${GENERATED_INSTALL_DIR})

CanopyGenerate(
  test_types
  serialiser_test/test_types.idl
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated
  ""
  yas_binary
  yas_compressed_binary
  yas_json
  protocol_buffers
  canonical_crypto
  include_paths ${CMAKE_CURRENT_SOURCE_DIR}/.)

CanopyGenerate(
  io_uring_test
  io_uring_test/test.idl
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated
  ""
  yas_binary
  yas_compressed_binary
  yas_json
  protocol_buffers
  include_paths ${CMAKE_CURRENT_SOURCE_DIR}/.)

if(CANOPY_BUILD_SGX_ATTESTATION_TESTS)
  CanopyGenerate(
    attestation_test
    attestation_test/attestation_test.idl
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_BINARY_DIR}/generated
    ""
    yas_binary
    yas_compressed_binary
    yas_json
    protocol_buffers
    canonical_crypto
    include_paths ${CMAKE_CURRENT_SOURCE_DIR}/.)
endif()
