#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

function(CanopyGenerateConnectionConfigSchema idl_target output_dir)
  set(options)
  set(oneValueArgs TARGET OUTPUT_SCHEMA ROOT_DEFINITION)
  set(multiValueArgs)
  cmake_parse_arguments(
    SCHEMA
    "${options}"
    "${oneValueArgs}"
    "${multiValueArgs}"
    ${ARGN})

  if(NOT TARGET ${idl_target})
    message(FATAL_ERROR "CanopyGenerateConnectionConfigSchema expected an IDL CMake target, got '${idl_target}'")
  endif()

  string(REGEX REPLACE "_idl$" "" module_name "${idl_target}")
  if("${module_name}" STREQUAL "${idl_target}")
    message(FATAL_ERROR "CanopyGenerateConnectionConfigSchema expected '${idl_target}' to end in _idl")
  endif()

  if(NOT SCHEMA_TARGET)
    set(SCHEMA_TARGET "${module_name}_connection_schema")
  endif()

  if(NOT SCHEMA_OUTPUT_SCHEMA)
    set(SCHEMA_OUTPUT_SCHEMA "${module_name}.schema.json")
  endif()

  set(app_schema "${CMAKE_BINARY_DIR}/generated/json_schema/${module_name}/${module_name}.json")
  set(output_file "${output_dir}/${SCHEMA_OUTPUT_SCHEMA}")
  set(root_definition_args)
  if(SCHEMA_ROOT_DEFINITION)
    set(root_definition_args --root-definition "${SCHEMA_ROOT_DEFINITION}")
  endif()

  add_custom_command(
    OUTPUT "${output_file}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${output_dir}"
    COMMAND
      "$<TARGET_FILE:config_schema_compose>" --app-schema "${app_schema}" --generated-schema-dir
      "${CMAKE_BINARY_DIR}/generated/json_schema" --output-dir "${output_dir}" --output-schema "${SCHEMA_OUTPUT_SCHEMA}"
      ${root_definition_args}
    DEPENDS config_schema_compose ${idl_target}
    COMMENT "Composing connection config schema ${output_file}"
    VERBATIM)

  add_custom_target(${SCHEMA_TARGET} DEPENDS "${output_file}")
  set(${SCHEMA_TARGET}_FILE
      "${output_file}"
      PARENT_SCOPE)
endfunction()
