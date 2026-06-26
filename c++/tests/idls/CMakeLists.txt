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
  canonical_crypto
  mock example/example_mock.h
  dependencies example_import_idl example_shared_idl
  include_paths ${CMAKE_CURRENT_SOURCE_DIR}/.
  install_dir ${GENERATED_INSTALL_DIR})

function(canopy_assert_idl_generate_property_contains target property expected_value)
  if(NOT TARGET ${target})
    message(FATAL_ERROR "Expected IDL generate target '${target}' to exist")
  endif()

  get_target_property(property_values ${target} ${property})
  if(NOT property_values)
    message(FATAL_ERROR "Expected target '${target}' property '${property}' to contain '${expected_value}'")
  endif()

  list(FIND property_values "${expected_value}" found_index)
  if(found_index EQUAL -1)
    message(
      FATAL_ERROR
        "Expected target '${target}' property '${property}' to contain '${expected_value}', got '${property_values}'")
  endif()
endfunction()

# Regression guard for CanopyGenerate(... dependencies ...): callers should list direct IDL dependencies only.
# Transitive IDL files and search paths must be propagated through the dependency targets so imported-schema edits
# rebuild the final generated target without duplicating dependencies here.
canopy_assert_idl_generate_property_contains(example_import_idl_generate idl_file_dependencies
                                             "${CANOPY_INTERFACE_SOURCE_DIR}/json/json.idl")
canopy_assert_idl_generate_property_contains(example_idl_generate idl_file_dependencies
                                             "${CANOPY_INTERFACE_SOURCE_DIR}/json/json.idl")
canopy_assert_idl_generate_property_contains(example_idl_generate idl_search_paths "${CANOPY_INTERFACE_SOURCE_DIR}")

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
  schema_cycle
  schema_cycle/schema_cycle.idl
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated
  ""
  yas_binary
  include_paths ${CMAKE_CURRENT_SOURCE_DIR}/.)

CanopyGenerate(
  set_fixture
  set_fixture/set_fixture.idl
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated
  ""
  yas_binary
  yas_json
  include_paths ${CMAKE_CURRENT_SOURCE_DIR}/.)

CanopyGenerate(
  nullable_fixture
  nullable_fixture/nullable_fixture.idl
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated
  ""
  yas_binary
  yas_json
  protocol_buffers
  nanopb
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

CanopyGenerate(
  rest_fixture
  rest_fixture/rest_fixture.idl
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated
  ""
  yas_json
  rest_client
  ${CMAKE_CURRENT_SOURCE_DIR}/rest_fixture/rest_fixture.openapi.json
  dependencies rpc_types_idl
  include_paths ${CMAKE_CURRENT_SOURCE_DIR}/. ${CANOPY_INTERFACE_SOURCE_DIR}
  install_dir ${GENERATED_INSTALL_DIR})
