#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

# Capture this directory at include time — CMAKE_CURRENT_LIST_DIR inside a function refers to the caller's directory,
# not the file where the function is defined.
set(_CANOPY_GENERATE_CMAKE_DIR
    "${CMAKE_CURRENT_LIST_DIR}"
    CACHE INTERNAL "")

function(
  CanopyGenerate
  name
  idl
  base_dir
  output_path
  namespace)
  set(options
      suppress_catch_stub_exceptions
      no_include_rpc_headers
      yas_binary
      yas_compressed_binary
      yas_json
      protocol_buffers
      nanopb
      canonical_crypto)
  set(singleValueArgs mock install_dir rest_client)
  set(multiValueArgs
      dependencies
      link_libraries
      include_paths
      defines
      additional_headers
      rethrow_stub_exception
      additional_stub_header)

  # split out multivalue variables
  cmake_parse_arguments(
    "params"
    "${options}"
    "${singleValueArgs}"
    "${multiValueArgs}"
    ${ARGN})

  if(params_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "Unknown CanopyGenerate arguments for '${name}': ${params_UNPARSED_ARGUMENTS}")
  endif()

  # Define cache variables for global settings with defaults These allow users to override settings while providing
  # sensible defaults
  get_filename_component(_canopy_source_root "${_CANOPY_GENERATE_CMAKE_DIR}/.." ABSOLUTE)
  if(NOT DEFINED CANOPY_SCHEMA_ID_BASE)
    set(CANOPY_SCHEMA_ID_BASE
        "https://schemas.canopy.dev/"
        CACHE STRING "Base URI prepended to generated JSON Schema $id paths")
  endif()
  if(NOT DEFINED CANOPY_DEFINES)
    set(CANOPY_DEFINES
        ""
        CACHE STRING "Host compile definitions")
  endif()
  if(NOT DEFINED CANOPY_INCLUDES)
    set(CANOPY_INCLUDES
        ""
        CACHE STRING "Host include directories")
  endif()
  if(NOT DEFINED CANOPY_COMPILE_OPTIONS)
    set(CANOPY_COMPILE_OPTIONS
        ""
        CACHE STRING "Host compile options")
  endif()
  if(NOT DEFINED CANOPY_FMT_LIB)
    set(CANOPY_FMT_LIB
        ""
        CACHE STRING "Host fmt library")
  endif()
  if(NOT DEFINED CANOPY_NANOPB_GENERATOR)
    set(CANOPY_NANOPB_GENERATOR
        "${_canopy_source_root}/c++/submodules/nanopb/generator/nanopb_generator.py"
        CACHE FILEPATH "Nanopb generator executable")
  endif()
  if(NOT DEFINED CANOPY_PROTOBUF_PYTHON_SOURCE_DIR)
    set(CANOPY_PROTOBUF_PYTHON_SOURCE_DIR
        "${_canopy_source_root}/submodules/protobuf/python"
        CACHE PATH "protobuf Python runtime source directory used by Nanopb generator")
  endif()
  if(NOT DEFINED CANOPY_PROTOBUF_SOURCE_PROTO_DIR)
    set(CANOPY_PROTOBUF_SOURCE_PROTO_DIR
        "${_canopy_source_root}/submodules/protobuf/src"
        CACHE PATH "protobuf source .proto directory used by Nanopb generator")
  endif()
  if(NOT DEFINED CANOPY_BUILD_NANOPB)
    set(CANOPY_BUILD_NANOPB
        ON
        CACHE BOOL "Include Nanopb support")
  endif()
  if(NOT DEFINED CANOPY_REDUCE_THIRD_PARTY_REST_DEBUG_INFO)
    set(CANOPY_REDUCE_THIRD_PARTY_REST_DEBUG_INFO
        ON
        CACHE BOOL "Compile generated third-party REST sources with reduced debug info in Debug builds")
  endif()
  if(NOT DEFINED CANOPY_BUILD_CANONICAL_CRYPTO)
    set(CANOPY_BUILD_CANONICAL_CRYPTO
        ON
        CACHE BOOL "Include deterministic canonical_crypto serialization support")
  endif()
  if(NOT DEFINED CANOPY_NANOPB_PROTOC_EXECUTABLE)
    if(TARGET protoc)
      set(CANOPY_NANOPB_PROTOC_EXECUTABLE "$<TARGET_FILE:protoc>")
    else()
      find_program(CANOPY_NANOPB_PROTOC_PROGRAM protoc)
      if(CANOPY_NANOPB_PROTOC_PROGRAM)
        set(CANOPY_NANOPB_PROTOC_EXECUTABLE
            "${CANOPY_NANOPB_PROTOC_PROGRAM}"
            CACHE FILEPATH "protoc executable used by Nanopb generator")
      else()
        set(CANOPY_NANOPB_PROTOC_EXECUTABLE
            ""
            CACHE FILEPATH "protoc executable used by Nanopb generator")
      endif()
    endif()
  endif()
  if(NOT DEFINED CANOPY_WARN_OK)
    set(CANOPY_WARN_OK
        ""
        CACHE STRING "Warning flags that are acceptable")
  endif()
  # Extract directory and base filename from IDL path BEFORE converting to absolute idl parameter is like
  # "example_shared/example_shared.idl" or "rpc/rpc_types.idl" or just "example.idl"
  get_filename_component(idl_dir ${idl} DIRECTORY)
  get_filename_component(idl_basename ${idl} NAME_WE)

  # The subdirectory is extracted from the IDL path If idl has no directory(e.g., "example.idl"), use empty subdirectory
  if("${idl_dir}" STREQUAL "")
    set(sub_directory ".")
  else()
    set(sub_directory ${idl_dir})
  endif()

  # The base filename comes from the IDL filename(without.idl extension)
  set(base_filename ${idl_basename})

  # Generator receives only the basename The directory will be extracted from the IDL path by the generator
  set(base_name ${base_filename})

  # keep relative path of idl for install, else use only the file name
  cmake_path(IS_RELATIVE idl idl_is_relative)

  if(${idl_is_relative})
    cmake_path(GET idl PARENT_PATH idl_relative_dir)
    cmake_path(
      APPEND
      base_dir
      ${idl}
      OUTPUT_VARIABLE
      idl)
  else()
    set(idl_relative_dir .)
  endif()

  set(_canopy_rest_generate_target "")
  if(NOT "${params_rest_client}" STREQUAL "" AND "${params_rest_client}" MATCHES "\\.(openapi|swagger)\\.json$")
    get_filename_component(_canopy_rest_converter "${_canopy_source_root}/tools/openapi_to_canopy_idl.py" ABSOLUTE)
    find_package(
      Python3
      COMPONENTS Interpreter
      REQUIRED)
    get_filename_component(_canopy_generate_base_dir_abs "${base_dir}" ABSOLUTE)

    cmake_path(IS_RELATIVE params_rest_client _canopy_rest_spec_is_relative)
    if(_canopy_rest_spec_is_relative)
      cmake_path(
        APPEND
        "${_canopy_generate_base_dir_abs}"
        ${params_rest_client}
        OUTPUT_VARIABLE
        _canopy_rest_spec)
    else()
      set(_canopy_rest_spec "${params_rest_client}")
    endif()
    get_filename_component(_canopy_rest_spec_abs "${_canopy_rest_spec}" ABSOLUTE)
    get_filename_component(_canopy_rest_spec_dir "${_canopy_rest_spec_abs}" DIRECTORY)
    get_filename_component(_canopy_rest_spec_name "${_canopy_rest_spec_abs}" NAME)
    string(REGEX REPLACE "\\.(openapi|swagger)\\.json$" "" _canopy_rest_spec_stem "${_canopy_rest_spec_name}")
    set(_canopy_rest_binding_abs "${_canopy_rest_spec_dir}/${_canopy_rest_spec_stem}.rest.json")

    set(_canopy_rest_overlay_args "")
    set(_canopy_rest_overlay_deps "")
    set(_canopy_rest_default_overlay "${_canopy_rest_spec_dir}/${_canopy_rest_spec_stem}.canopy.overlay.json")
    if(EXISTS "${_canopy_rest_default_overlay}")
      list(APPEND _canopy_rest_overlay_args --overlay "${_canopy_rest_default_overlay}")
      list(APPEND _canopy_rest_overlay_deps "${_canopy_rest_default_overlay}")
    endif()

    add_custom_command(
      OUTPUT "${idl}" "${_canopy_rest_binding_abs}"
      COMMAND ${Python3_EXECUTABLE} "${_canopy_rest_converter}" --input "${_canopy_rest_spec_abs}"
              ${_canopy_rest_overlay_args} --output "${idl}" --binding "${_canopy_rest_binding_abs}"
      MAIN_DEPENDENCY "${_canopy_rest_spec_abs}"
      DEPENDS "${_canopy_rest_converter}" ${_canopy_rest_overlay_deps}
      COMMENT "Converting REST interface ${_canopy_rest_spec_abs}"
      VERBATIM)
    add_custom_target(${name}_rest_generate DEPENDS "${idl}" "${_canopy_rest_binding_abs}")
    set_source_files_properties("${idl}" "${_canopy_rest_binding_abs}" PROPERTIES GENERATED TRUE)
    set_property(
      DIRECTORY
      APPEND
      PROPERTY CMAKE_CONFIGURE_DEPENDS "${_canopy_rest_spec_abs}" ${_canopy_rest_overlay_deps}
               "${_canopy_rest_converter}")
    set(params_rest_client "${_canopy_rest_binding_abs}")
    set(_canopy_rest_generate_target "${name}_rest_generate")
  endif()

  # Construct individual paths for CMake dependency tracking
  set(header_path ${sub_directory}/${base_filename}.h)
  set(proxy_path ${sub_directory}/${base_filename}_proxy.cpp)
  set(stub_path ${sub_directory}/${base_filename}_stub.cpp)
  set(stub_header_path ${sub_directory}/${base_filename}_stub.h)
  set(full_header_path ${output_path}/include/${header_path})
  set(json_schema_header_path ${sub_directory}/${base_filename}_schema.h)
  set(full_json_schema_header_path ${output_path}/include/${json_schema_header_path})
  set(full_proxy_path ${output_path}/src/${proxy_path})
  set(full_stub_path ${output_path}/src/${stub_path})
  set(full_stub_header_path ${output_path}/include/${stub_header_path})
  # Determine which serialization formats to generate
  set(generate_yas FALSE)
  set(generate_protobuf FALSE)
  set(generate_nanopb FALSE)
  set(generate_canonical_crypto FALSE)
  set(generate_rest_client FALSE)

  if(${params_yas_binary}
     OR ${params_yas_compressed_binary}
     OR ${params_yas_json})
    set(generate_yas TRUE)
    set(yas_path ${sub_directory}/yas/${base_filename}.cpp)
    set(full_yas_path ${output_path}/src/${yas_path})
  else()
    set(yas_path "")
    set(full_yas_path "")
  endif()

  if(${params_protocol_buffers})
    set(protobuf_family_generated FALSE)
    if(CANOPY_BUILD_PROTOCOL_BUFFERS)
      set(generate_protobuf TRUE)
      set(protobuf_family_generated TRUE)
    elseif(generate_yas)
      message(STATUS "Protocol Buffers generation requested for '${name}', but CANOPY_BUILD_PROTOCOL_BUFFERS is OFF.")
    endif()

    if(CANOPY_BUILD_NANOPB)
      set(generate_nanopb TRUE)
      set(protobuf_family_generated TRUE)
    endif()

    if(NOT protobuf_family_generated AND NOT generate_yas)
      message(
        FATAL_ERROR
          "Protocol Buffers generation was requested for '${name}', but CANOPY_BUILD_PROTOCOL_BUFFERS and CANOPY_BUILD_NANOPB are OFF and no alternative generated serialization format was requested."
      )
    endif()
  endif()

  if(${params_nanopb})
    if(CANOPY_BUILD_NANOPB)
      set(generate_nanopb TRUE)
    elseif(CANOPY_BUILD_PROTOCOL_BUFFERS)
      set(generate_protobuf TRUE)
      message(STATUS "Nanopb generation requested for '${name}', but CANOPY_BUILD_NANOPB is OFF. "
                     "Using the Protocol Buffers backend for rpc::encoding::nanopb.")
    elseif(generate_yas)
      message(STATUS "Nanopb generation requested for '${name}', but CANOPY_BUILD_NANOPB is OFF. "
                     "Continuing with the requested YAS format(s) only.")
    else()
      message(FATAL_ERROR "Nanopb generation was requested for '${name}', but CANOPY_BUILD_NANOPB is OFF "
                          "and no alternative generated serialization format was requested.")
    endif()
  endif()

  if(${params_canonical_crypto})
    if(CANOPY_BUILD_CANONICAL_CRYPTO)
      set(generate_canonical_crypto TRUE)
    elseif(
      NOT generate_yas
      AND NOT generate_protobuf
      AND NOT generate_nanopb)
      message(
        FATAL_ERROR "canonical_crypto generation was requested for '${name}', but CANOPY_BUILD_CANONICAL_CRYPTO is OFF "
                    "and no alternative generated serialization format was requested.")
    else()
      message(STATUS "canonical_crypto generation requested for '${name}', but CANOPY_BUILD_CANONICAL_CRYPTO is OFF. "
                     "Continuing with the other requested serialization format(s).")
    endif()
  endif()

  if(NOT "${params_rest_client}" STREQUAL "")
    if(NOT ${params_yas_json})
      message(FATAL_ERROR "rest_client generation for '${name}' requires yas_json generation.")
    endif()
    set(generate_rest_client TRUE)
    set(rest_metadata_path "${params_rest_client}")
    set(rest_path ${sub_directory}/${base_filename}_rest.cpp)
    set(full_rest_path ${output_path}/src/${rest_path})
  else()
    set(rest_metadata_path "")
    set(rest_path "")
    set(full_rest_path "")
  endif()

  set(reduce_generated_rest_debug_info FALSE)
  if(CANOPY_REDUCE_THIRD_PARTY_REST_DEBUG_INFO AND generate_rest_client)
    get_filename_component(canopy_third_party_interfaces_dir "${_canopy_source_root}/third_party_interfaces" ABSOLUTE)
    get_filename_component(canopy_generate_base_dir "${base_dir}" ABSOLUTE)
    file(RELATIVE_PATH canopy_generate_base_rel "${canopy_third_party_interfaces_dir}" "${canopy_generate_base_dir}")
    cmake_path(IS_ABSOLUTE canopy_generate_base_rel canopy_generate_base_rel_is_absolute)
    if(NOT canopy_generate_base_rel MATCHES "^\\.\\." AND NOT canopy_generate_base_rel_is_absolute)
      set(reduce_generated_rest_debug_info TRUE)
    endif()
  endif()

  if(generate_protobuf OR generate_nanopb)
    set(protobuf_path ${sub_directory}/protobuf/${base_filename}.proto)
    set(full_protobuf_path ${output_path}/src/${protobuf_path})
    set(protobuf_cpp_path ${sub_directory}/protobuf/${base_filename}.cpp)
    set(full_protobuf_cpp_path ${output_path}/src/${protobuf_cpp_path})
    set(nanopb_cpp_path ${sub_directory}/protobuf/${base_filename}_nanopb.cpp)
    set(full_nanopb_cpp_path ${output_path}/src/${nanopb_cpp_path})
    set(protobuf_manifest_path ${sub_directory}/protobuf/manifest.txt)
    set(full_protobuf_manifest_path ${output_path}/src/${protobuf_manifest_path})
  else()
    set(protobuf_path "")
    set(full_protobuf_path "")
    set(protobuf_cpp_path "")
    set(full_protobuf_cpp_path "")
    set(nanopb_cpp_path "")
    set(full_nanopb_cpp_path "")
    set(protobuf_manifest_path "")
    set(full_protobuf_manifest_path "")
  endif()

  if(${CANOPY_VERBOSE_GENERATOR})
    message("CanopyGenerate name ${name}")
    message("idl ${idl}")
    message("base_dir ${base_dir}")
    message("output_path ${output_path}")
    message("base_name ${base_name}")
    message("namespace ${namespace}")
    message("suppress_catch_stub_exceptions ${params_suppress_catch_stub_exceptions}")
    message("install_dir ${params_install_dir}")
    message("dependencies ${params_dependencies}")
    message("link_libraries ${params_link_libraries}")
    message("additional_headers ${params_additional_headers}")
    message("rethrow_stub_exception ${params_rethrow_stub_exception}")
    message("additional_stub_header ${params_additional_stub_header}")
    message("paths ${params_include_paths}")
    message("defines ${params_defines}")
    message("mock ${params_mock}")
    message("header_path ${header_path}")
    message("json_schema_header_path ${json_schema_header_path}")
    message("proxy_path ${proxy_path}")
    message("stub_path ${stub_path}")
    message("stub_header_path ${stub_header_path}")
    message("full_header_path ${full_header_path}")
    message("full_json_schema_header_path ${full_json_schema_header_path}")
    message("full_proxy_path ${full_proxy_path}")
    message("full_stub_path ${full_stub_path}")
    message("full_stub_header_path ${full_stub_header_path}")
    message("yas_path ${yas_path}")
    message("full_yas_path ${full_yas_path}")
    message("protobuf_path ${protobuf_path}")
    message("full_protobuf_path ${full_protobuf_path}")
    message("protobuf_cpp_path ${protobuf_cpp_path}")
    message("full_protobuf_cpp_path ${full_protobuf_cpp_path}")
    message("generate_canonical_crypto ${generate_canonical_crypto}")
    message("generate_rest_client ${generate_rest_client}")
    message("rest_client ${rest_metadata_path}")
    message("no_include_rpc_headers ${params_no_include_rpc_headers}")
  endif()

  # Use cache variable for generator executable with fallback
  if(NOT DEFINED CANOPY_IDL_GENERATOR_EXECUTABLE)
    set(CANOPY_IDL_GENERATOR_EXECUTABLE
        "generator"
        CACHE STRING "Path to the IDL generator executable")
  endif()
  set(IDL_GENERATOR ${CANOPY_IDL_GENERATOR_EXECUTABLE})

  set(PATHS_PARAMS "")
  set(IDL_SEARCH_PATHS "")
  set(ADDITIONAL_HEADERS "")
  set(RETHROW_STUB_EXCEPTION "")
  set(ADDITIONAL_STUB_HEADER "")
  set(GENERATED_DEPENDENCIES "")
  set(IDL_FILE_DEPENDENCIES "")

  foreach(path ${params_include_paths})
    list(APPEND IDL_SEARCH_PATHS "${path}")
  endforeach()

  foreach(define ${params_defines})
    set(PATHS_PARAMS ${PATHS_PARAMS} -D "${define}")
  endforeach()

  foreach(additional_headers ${params_additional_headers})
    set(ADDITIONAL_HEADERS ${ADDITIONAL_HEADERS} --additional_headers "${additional_headers}")
  endforeach()

  foreach(rethrow ${params_rethrow_stub_exception})
    set(RETHROW_STUB_EXCEPTION ${RETHROW_STUB_EXCEPTION} --rethrow_stub_exception "${rethrow}")
  endforeach()

  foreach(stub_header ${params_additional_stub_header})
    set(ADDITIONAL_STUB_HEADER ${ADDITIONAL_STUB_HEADER} --additional_stub_header "${stub_header}")
  endforeach()

  foreach(dep ${params_dependencies})
    if(TARGET ${dep}_generate)
      get_target_property(dep_base_dir ${dep}_generate base_dir)

      if(dep_base_dir)
        list(APPEND IDL_SEARCH_PATHS "${dep_base_dir}")
      endif()

      set(GENERATED_DEPENDENCIES ${GENERATED_DEPENDENCIES} ${dep}_generate)
      get_target_property(dep_idl_search_paths ${dep}_generate idl_search_paths)
      if(dep_idl_search_paths)
        list(APPEND IDL_SEARCH_PATHS ${dep_idl_search_paths})
      endif()
      get_target_property(dep_idl_path ${dep}_generate idl_path)
      if(dep_idl_path)
        list(APPEND IDL_FILE_DEPENDENCIES "${dep_idl_path}")
      endif()
      get_target_property(dep_idl_file_dependencies ${dep}_generate idl_file_dependencies)
      if(dep_idl_file_dependencies)
        list(APPEND IDL_FILE_DEPENDENCIES ${dep_idl_file_dependencies})
      endif()
    else()
      message("target ${dep}_generate does not exist so skipped")
    endif()
    # when installed(used through a package) idl dependencies can be found through their targets : we know that <
    # package_dir> / ${param_install_dir } / interfaces / include is in the target's include directories, and that the
    # idls themselves are in < package_dir> / ${param_install_dir }
    if(TARGET ${dep})
      get_target_property(include_dirs ${dep} INTERFACE_INCLUDE_DIRECTORIES)
      foreach(include_dir ${include_dirs})
        if(${include_dir} MATCHES "/interfaces/include$")
          string(REPLACE "/interfaces/include" "" idl_dir ${include_dir})
          list(APPEND IDL_SEARCH_PATHS "${idl_dir}")
        endif()
      endforeach()
    endif()
  endforeach()

  list(REMOVE_DUPLICATES IDL_SEARCH_PATHS)
  list(REMOVE_DUPLICATES IDL_FILE_DEPENDENCIES)
  set(IDL_EXPORTED_SEARCH_PATHS "${base_dir}" ${IDL_SEARCH_PATHS})
  list(REMOVE_DUPLICATES IDL_EXPORTED_SEARCH_PATHS)
  set_property(
    DIRECTORY
    APPEND
    PROPERTY CMAKE_CONFIGURE_DEPENDS ${idl} ${IDL_FILE_DEPENDENCIES})

  foreach(path ${IDL_SEARCH_PATHS})
    set(PATHS_PARAMS ${PATHS_PARAMS} --path "${path}")
  endforeach()

  if(NOT ${namespace} STREQUAL "")
    set(PATHS_PARAMS ${PATHS_PARAMS} --namespace "${namespace}")
  endif()

  if(DEFINED params_mock AND NOT ${params_mock} STREQUAL "")
    set(PATHS_PARAMS ${PATHS_PARAMS} --mock "${params_mock}")
  endif()

  if(${params_no_include_rpc_headers})
    set(PATHS_PARAMS ${PATHS_PARAMS} --no_include_rpc_headers)
  endif()

  if(${params_suppress_catch_stub_exceptions})
    set(PATHS_PARAMS ${PATHS_PARAMS} --suppress_catch_stub_exceptions)
  endif()

  set(proto_proxy_src "")
  if(generate_protobuf)
    # Keep the placeholder proxy source beside the other generated outputs so all targets refer to a single stable path
    # across source directories.
    set(proto_proxy_src "${output_path}/src/${sub_directory}/${base_name}_proto_proxy.cpp")
  endif()

  # Build the list of output files based on enabled formats
  set(GENERATOR_OUTPUTS
      ${full_header_path}
      ${full_json_schema_header_path}
      ${full_proxy_path}
      ${full_stub_header_path}
      ${full_stub_path})

  if(generate_yas)
    list(APPEND GENERATOR_OUTPUTS ${full_yas_path})
  endif()

  if(generate_rest_client)
    list(APPEND GENERATOR_OUTPUTS ${full_rest_path})
  endif()

  if(generate_protobuf OR generate_nanopb)
    # Only the manifest and wrapper C++ files are direct generator outputs.Individual.proto files are listed in
    # manifest.txt and compiled separately.
    list(APPEND GENERATOR_OUTPUTS ${full_protobuf_manifest_path})
    if(generate_protobuf)
      list(APPEND GENERATOR_OUTPUTS ${full_protobuf_cpp_path} ${proto_proxy_src})
    endif()
    if(generate_nanopb)
      list(APPEND GENERATOR_OUTPUTS ${full_nanopb_cpp_path})
    endif()
  endif()
  if(generate_protobuf)
    set(GENERATOR_POST_COMMANDS COMMAND ${CMAKE_COMMAND} -E touch ${proto_proxy_src})
  else()
    set(GENERATOR_POST_COMMANDS "")
  endif()

  set_source_files_properties(${GENERATOR_OUTPUTS} PROPERTIES GENERATED TRUE)
  if(reduce_generated_rest_debug_info)
    set(canopy_third_party_rest_debug_options
        "$<$<AND:$<CONFIG:Debug>,$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang>>:-gline-tables-only>"
        "$<$<AND:$<CONFIG:Debug>,$<COMPILE_LANG_AND_ID:CXX,GNU>>:-g1>")
    set(canopy_third_party_rest_generated_sources ${full_proxy_path} ${full_stub_path} ${full_rest_path})
    if(generate_yas)
      list(APPEND canopy_third_party_rest_generated_sources ${full_yas_path})
    endif()
    set_source_files_properties(${canopy_third_party_rest_generated_sources}
                                PROPERTIES COMPILE_OPTIONS "${canopy_third_party_rest_debug_options}")
  endif()

  # Build generator command with conditional serialization flags
  set(SERIALIZATION_FLAGS "")
  if(generate_yas)
    if(${params_yas_binary})
      set(SERIALIZATION_FLAGS ${SERIALIZATION_FLAGS} --yas_binary)
    endif()
    if(${params_yas_compressed_binary})
      set(SERIALIZATION_FLAGS ${SERIALIZATION_FLAGS} --yas_compressed_binary)
    endif()
    if(${params_yas_json})
      set(SERIALIZATION_FLAGS ${SERIALIZATION_FLAGS} --yas_json)
    endif()
  endif()

  if(generate_protobuf)
    set(SERIALIZATION_FLAGS ${SERIALIZATION_FLAGS} --protobuf)
  endif()
  if(generate_nanopb)
    set(SERIALIZATION_FLAGS ${SERIALIZATION_FLAGS} --nanopb)
  endif()
  if(generate_canonical_crypto)
    set(SERIALIZATION_FLAGS ${SERIALIZATION_FLAGS} --canonical_crypto)
  endif()
  if(generate_rest_client)
    set(SERIALIZATION_FLAGS ${SERIALIZATION_FLAGS} --rest_client ${rest_metadata_path})
  endif()

  # Determine generator dependency for custom command If generator target exists(building from source), depend on the
  # executable file Otherwise use the cached executable path(installed version)
  set(GENERATOR_DEPENDENCY "")
  if(TARGET generator)
    set(GENERATOR_DEPENDENCY $<TARGET_FILE:generator>)
  endif()

  message(
    "  add_custom_command(
    OUTPUT ${GENERATOR_OUTPUTS}
    COMMAND
	    ${IDL_GENERATOR} --idl ${idl} --output_path ${output_path} --name ${base_name}
    --schema_id_base ${CANOPY_SCHEMA_ID_BASE} ${SERIALIZATION_FLAGS} ${PATHS_PARAMS} ${ADDITIONAL_HEADERS}
    ${RETHROW_STUB_EXCEPTION} ${ADDITIONAL_STUB_HEADER}
    MAIN_DEPENDENCY ${idl}
    IMPLICIT_DEPENDS ${idl}
    DEPENDS ${IDL_FILE_DEPENDENCIES} ${GENERATOR_DEPENDENCY} ${rest_metadata_path})")

  add_custom_command(
    OUTPUT ${GENERATOR_OUTPUTS}
    COMMAND
      ${IDL_GENERATOR} --idl ${idl} --output_path ${output_path} --name ${base_name} --schema_id_base
      ${CANOPY_SCHEMA_ID_BASE} ${SERIALIZATION_FLAGS} ${PATHS_PARAMS} ${ADDITIONAL_HEADERS} ${RETHROW_STUB_EXCEPTION}
      ${ADDITIONAL_STUB_HEADER} ${GENERATOR_POST_COMMANDS}
    MAIN_DEPENDENCY ${idl}
    IMPLICIT_DEPENDS ${idl}
    DEPENDS ${IDL_FILE_DEPENDENCIES} ${GENERATOR_DEPENDENCY} ${rest_metadata_path}
    COMMENT "Running generator ${idl}")

  if(${CANOPY_VERBOSE_GENERATOR})
    message(
      "
	    ${IDL_GENERATOR} --idl ${idl} --output_path ${output_path} --name ${base_name}
    --schema_id_base ${CANOPY_SCHEMA_ID_BASE} ${SERIALIZATION_FLAGS} ${PATHS_PARAMS} ${ADDITIONAL_HEADERS}
	    ${RETHROW_STUB_EXCEPTION} ${ADDITIONAL_STUB_HEADER}
  ")
  endif()

  add_custom_target(${name}_idl_generate DEPENDS ${GENERATOR_OUTPUTS})

  # Ensure generator executable is built before generating IDL when Canopy is being built from source. Installed-package
  # consumers may provide only CANOPY_IDL_GENERATOR_EXECUTABLE.
  if(TARGET generator)
    add_dependencies(${name}_idl_generate generator)
  endif()
  if(NOT "${_canopy_rest_generate_target}" STREQUAL "")
    add_dependencies(${name}_idl_generate ${_canopy_rest_generate_target})
  endif()

  set_target_properties(
    ${name}_idl_generate
    PROPERTIES base_dir "${base_dir}"
               idl_path "${idl}"
               idl_file_dependencies "${IDL_FILE_DEPENDENCIES}"
               idl_search_paths "${IDL_EXPORTED_SEARCH_PATHS}")

  # Only compile.proto files if protobuf - compatible formats are enabled
  if(generate_protobuf OR generate_nanopb)
    if("${CANOPY_NANOPB_PROTOC_EXECUTABLE}" STREQUAL "" AND TARGET protoc)
      set(CANOPY_NANOPB_PROTOC_EXECUTABLE "$<TARGET_FILE:protoc>")
    endif()
    if(generate_protobuf AND NOT TARGET protoc)
      message(FATAL_ERROR "Protocol Buffers generation for '${name}' requires the bundled protoc target.")
    endif()
    if(generate_nanopb AND "${CANOPY_NANOPB_PROTOC_EXECUTABLE}" STREQUAL "")
      message(
        FATAL_ERROR
          "Nanopb generation for '${name}' requires protoc. Set CANOPY_NANOPB_PROTOC_EXECUTABLE or enable CANOPY_BUILD_PROTOCOL_BUFFERS to build the bundled protoc tool."
      )
    endif()

    set(proto_dir ${output_path}/src/${sub_directory}/protobuf)
    set(PROTO_MANIFEST "${proto_dir}/manifest.txt")
    # Expose proto location so JS consumers(CanopyJavascriptGenerate) can copy.proto files
    set_target_properties(${name}_idl_generate PROPERTIES proto_dir "${proto_dir}" proto_src_root "${output_path}/src")

    # The stamp file tracks when the internal compilation script has finished
    set(proto_stamp_file ${proto_dir}/.proto_compiled)

    # Generate a cmake file that lists all the expected.pb.cc files This is created during build after the proto files
    # are compiled
    set(proto_sources_cmake ${proto_dir}/generated_sources.cmake)
    set(nanopb_src_root ${output_path}/src/${sub_directory}/nanopb)
    set_target_properties(${name}_idl_generate PROPERTIES nanopb_src_root "${nanopb_src_root}")
    set(nanopb_include_roots ${nanopb_src_root})
    if(NOT "${sub_directory}" STREQUAL "rpc")
      list(APPEND nanopb_include_roots "${output_path}/src/rpc/nanopb")
    endif()
    foreach(dep ${params_dependencies})
      if(TARGET ${dep}_generate)
        get_target_property(dep_nanopb_include_roots ${dep}_generate nanopb_include_roots)
        if(dep_nanopb_include_roots)
          list(APPEND nanopb_include_roots ${dep_nanopb_include_roots})
        else()
          get_target_property(dep_nanopb_src_root ${dep}_generate nanopb_src_root)
          if(dep_nanopb_src_root)
            list(APPEND nanopb_include_roots "${dep_nanopb_src_root}")
          endif()
        endif()
      endif()
    endforeach()
    list(REMOVE_DUPLICATES nanopb_include_roots)
    set_target_properties(${name}_idl_generate PROPERTIES nanopb_include_roots "${nanopb_include_roots}")
    set(nanopb_stamp_file ${proto_dir}/.nanopb_compiled)
    set(nanopb_sources_cmake ${proto_dir}/generated_nanopb_sources.cmake)
    set(nanopb_compile_dependencies ${PROTO_MANIFEST} ${_CANOPY_GENERATE_CMAKE_DIR}/compile_nanopb_protos.cmake)
    list(
      APPEND
      nanopb_compile_dependencies
      ${CANOPY_PROTOBUF_SOURCE_PROTO_DIR}/google/protobuf/descriptor.proto
      ${CANOPY_PROTOBUF_SOURCE_PROTO_DIR}/google/protobuf/compiler/plugin.proto
      ${CANOPY_PROTOBUF_PYTHON_SOURCE_DIR}/google/protobuf/__init__.py
      ${CANOPY_PROTOBUF_PYTHON_SOURCE_DIR}/google/protobuf/internal/python_edition_defaults.py.template)

    if(generate_protobuf)
      add_custom_command(
        OUTPUT ${proto_stamp_file} ${proto_sources_cmake}
        # Compile the proto files and generate the cmake file with.pb.cc sources
        COMMAND
          ${CMAKE_COMMAND} -D PROTOC=$<TARGET_FILE:protoc> -D SUB_DIR=${sub_directory} -D PROTO_DIR=${proto_dir} -D
          OUTPUT_DIR=${output_path}/src -D PROTO_SOURCES_CMAKE=${proto_sources_cmake} -P
          ${_CANOPY_GENERATE_CMAKE_DIR}/compile_protos.cmake
        COMMAND ${CMAKE_COMMAND} -E touch ${proto_stamp_file}
        DEPENDS ${PROTO_MANIFEST} protoc
        COMMENT "Discovering and compiling .proto files for ${name}")

      add_custom_target(${name}_proto_compile DEPENDS ${proto_stamp_file})
    endif()

    if(generate_nanopb)
      add_custom_command(
        OUTPUT ${nanopb_stamp_file} ${nanopb_sources_cmake}
        COMMAND
          ${CMAKE_COMMAND} -D PROTOC=${CANOPY_NANOPB_PROTOC_EXECUTABLE} -D NANOPB_GENERATOR=${CANOPY_NANOPB_GENERATOR}
          -D PROTO_DIR=${proto_dir} -D OUTPUT_DIR=${output_path}/src -D CPP_OUT_DIR=${nanopb_src_root} -D
          NANOPB_SOURCES_CMAKE=${nanopb_sources_cmake} -D
          PROTOBUF_PYTHON_SOURCE_DIR=${CANOPY_PROTOBUF_PYTHON_SOURCE_DIR} -D
          PROTOBUF_SOURCE_PROTO_DIR=${CANOPY_PROTOBUF_SOURCE_PROTO_DIR} -P
          ${_CANOPY_GENERATE_CMAKE_DIR}/compile_nanopb_protos.cmake
        COMMAND ${CMAKE_COMMAND} -E touch ${nanopb_stamp_file}
        DEPENDS ${nanopb_compile_dependencies}
        COMMENT "Discovering and compiling Nanopb .proto files for ${name}")

      add_custom_target(${name}_nanopb_compile DEPENDS ${nanopb_stamp_file})
    endif()

    # Create a placeholder cmake file if it doesn't exist (for configure time)
    if(NOT EXISTS ${proto_sources_cmake})
      file(WRITE ${proto_sources_cmake}
           "# Placeholder - will be regenerated at build time\nset(PROTO_PB_SOURCES \"\")\n")
    endif()
    if(NOT EXISTS ${nanopb_sources_cmake})
      file(WRITE ${nanopb_sources_cmake}
           "# Placeholder - will be regenerated at build time\nset(NANOPB_PB_SOURCES \"\")\n")
    endif()

    # Read the manifest.txt file to determine which.pb.cc files will be generated The manifest.txt is created by the IDL
    # generator, so it exists after code generation
    set(PROTO_PB_SOURCES "")
    if(EXISTS ${PROTO_MANIFEST})

      # Read manifest.txt and compute corresponding.pb.cc filenames
      file(READ "${PROTO_MANIFEST}" MANIFEST_CONTENT)
      string(REGEX REPLACE "\n" ";" PROTO_FILE_NAMES "${MANIFEST_CONTENT}")

      foreach(PROTO_NAME ${PROTO_FILE_NAMES})
        string(STRIP "${PROTO_NAME}" PROTO_NAME)
        if(NOT "${PROTO_NAME}" STREQUAL "")
          # Convert example.proto->example.pb.cc
          string(REGEX REPLACE "\\.proto$" ".pb.cc" PB_CC_NAME "${PROTO_NAME}")
          message("set(pb_cc_file ${output_path}/src / ${PB_CC_NAME})")
          set(pb_cc_file "${output_path}/src/${PB_CC_NAME}")
          list(APPEND PROTO_PB_SOURCES "${pb_cc_file}")
          # Mark as GENERATED so CMake doesn 't complain that it doesn' t exist yet
          set_source_files_properties("${pb_cc_file}" PROPERTIES GENERATED TRUE)
        endif()
      endforeach()
    else()
      # Manifest doesn't exist at configure time (e.g., after deleting build/generated) Use the generated_sources.cmake
      # file which will be populated at build time
      if(EXISTS ${proto_sources_cmake})
        include(${proto_sources_cmake})
      endif()
    endif()

    set(NANOPB_PB_SOURCES "")
    if(generate_nanopb)
      if(EXISTS ${PROTO_MANIFEST})

        # Match the protobuf path: compute the generated nanopb C sources from manifest.txt at configure time when the
        # manifest is available, and compile those generated sources directly.
        file(READ "${PROTO_MANIFEST}" MANIFEST_CONTENT)
        string(REGEX REPLACE "\n" ";" PROTO_FILE_NAMES "${MANIFEST_CONTENT}")

        foreach(PROTO_NAME ${PROTO_FILE_NAMES})
          string(STRIP "${PROTO_NAME}" PROTO_NAME)
          if(NOT "${PROTO_NAME}" STREQUAL "")
            string(REGEX REPLACE "\\.proto$" ".pb.c" PB_C_NAME "${PROTO_NAME}")
            set(pb_c_file "${nanopb_src_root}/${PB_C_NAME}")
            list(APPEND NANOPB_PB_SOURCES "${pb_c_file}")
            set_source_files_properties("${pb_c_file}" PROPERTIES GENERATED TRUE)
          endif()
        endforeach()
      else()
        if(EXISTS ${nanopb_sources_cmake})
          include(${nanopb_sources_cmake})
        endif()
      endif()
    endif()

    # Mark Canopy-generated wrapper files as GENERATED. They intentionally remain visible to clang-tidy.
    if(generate_protobuf)
      set_source_files_properties(
        "${full_protobuf_cpp_path}" "${proto_proxy_src}"
        PROPERTIES GENERATED TRUE COMPILE_OPTIONS "${CANOPY_GENERATED_PROTOBUF_COMPILE_OPTIONS}")
    endif()
    if(generate_nanopb)
      set_source_files_properties("${full_nanopb_cpp_path}" PROPERTIES GENERATED TRUE COMPILE_OPTIONS
                                                                                      "-Wno-unreachable-code")
    endif()

    if(PROTO_PB_SOURCES)
      # Raw protoc sources include protobuf / abseil internals directly. Keep the owning Canopy targets pedantic, but
      # quarantine third-party generated translation-unit warnings here.
      set_source_files_properties(
        ${PROTO_PB_SOURCES}
        PROPERTIES GENERATED TRUE
                   SKIP_LINTING TRUE
                   COMPILE_OPTIONS "${CANOPY_GENERATED_PROTOBUF_COMPILE_OPTIONS}"
                   OBJECT_DEPENDS "${proto_stamp_file}")
    endif()

    if(NANOPB_PB_SOURCES)
      set_source_files_properties(
        ${NANOPB_PB_SOURCES}
        PROPERTIES GENERATED TRUE
                   SKIP_LINTING TRUE
                   COMPILE_OPTIONS "-Wno-unused-command-line-argument"
                   OBJECT_DEPENDS "${nanopb_stamp_file}")
    endif()

    if(generate_protobuf)
      # Define the library using the known wrapper, proxy, and all generated.pb.cc files
      message(
        "    add_library(${name}_protobuf_generated STATIC
      ${full_protobuf_cpp_path}
      ${proto_proxy_src}
      ${PROTO_PB_SOURCES})
")

      add_library(${name}_protobuf_generated STATIC ${full_protobuf_cpp_path} ${proto_proxy_src} ${PROTO_PB_SOURCES})

      # Add CANOPY compile definitions to ensure CANOPY_DEFAULT_ENCODING is available
      target_compile_definitions(${name}_protobuf_generated PRIVATE ${CANOPY_SHARED_DEFINES})

      # Make sure the generated protobuf C++ file waits for protobuf compilation
      if(EXISTS ${full_protobuf_cpp_path})
        add_dependencies(${name}_protobuf_generated ${name}_proto_compile)
      endif()

      # CRITICAL : We tell the library to include the object files found in the manifest This uses a generator
      # expression to pull in the objects compiled by our script
      target_link_libraries(${name}_protobuf_generated PRIVATE rpc::rpc)
      # Add protobuf include directories for compilation
      if(TARGET protobuf::libprotobuf)
        target_link_libraries(${name}_protobuf_generated PRIVATE protobuf::libprotobuf)
      endif()
      # Also add the generated protobuf directory for the.pb.h files
      target_include_directories(${name}_protobuf_generated SYSTEM PRIVATE ${proto_dir} ${output_path}/src)
      add_dependencies(${name}_protobuf_generated ${name}_proto_compile ${name}_idl_generate)
    endif()

    if(generate_nanopb)
      add_library(${name}_nanopb_generated STATIC ${full_nanopb_cpp_path} ${NANOPB_PB_SOURCES})
      target_compile_definitions(${name}_nanopb_generated PRIVATE ${CANOPY_SHARED_DEFINES})
      target_include_directories(${name}_nanopb_generated SYSTEM BEFORE
                                 PRIVATE ${nanopb_include_roots} ${output_path}/src ${proto_dir} ${output_path}/src)
      if(TARGET canopy_nanopb_runtime)
        target_link_libraries(${name}_nanopb_generated PRIVATE canopy_nanopb_runtime rpc::rpc)
      else()
        target_link_libraries(${name}_nanopb_generated PRIVATE rpc::rpc)
      endif()
      add_dependencies(${name}_nanopb_generated ${name}_nanopb_compile ${name}_idl_generate)
    endif()
  endif()

  # Build library sources based on enabled formats
  set(IDL_SOURCES
      ${full_header_path}
      ${full_json_schema_header_path}
      ${full_stub_header_path}
      ${full_stub_path}
      ${full_proxy_path})

  if(generate_yas)
    list(APPEND IDL_SOURCES ${full_yas_path})
  endif()

  if(generate_rest_client)
    list(APPEND IDL_SOURCES ${full_rest_path})
  endif()

  if(generate_protobuf)
    list(APPEND IDL_SOURCES ${full_protobuf_cpp_path} ${PROTO_PB_SOURCES})
  endif()

  # Create the host IDL library
  add_library(${name}_idl STATIC ${IDL_SOURCES})
  target_compile_definitions(${name}_idl PRIVATE ${CANOPY_DEFINES})
  if(generate_yas)
    target_compile_definitions(${name}_idl PUBLIC YAS_OBJECT_MAX_MEMBERS=256)
  endif()
  target_include_directories(
    ${name}_idl SYSTEM PUBLIC "$<BUILD_INTERFACE:${output_path}>" "$<BUILD_INTERFACE:${output_path}/include>"
                              "$<INSTALL_INTERFACE:include>")
  target_include_directories(${name}_idl SYSTEM PRIVATE "${output_path}" "${output_path}/include")
  target_include_directories(${name}_idl PRIVATE ${CANOPY_INCLUDES} ${params_include_paths})
  target_compile_options(${name}_idl PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
  set_property(TARGET ${name}_idl PROPERTY COMPILE_PDB_NAME ${name}_idl)

  if(CANOPY_ENABLE_CLANG_TIDY)
    set_target_properties(${name}_idl PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
  endif()

  target_link_libraries(${name}_idl PUBLIC rpc::rpc ${CANOPY_FMT_LIB} ${CANOPY_CORO_RUNTIME})

  # Link YAS if any YAS format is enabled
  if(generate_yas)
    target_link_libraries(${name}_idl PUBLIC yas_common)
  endif()
  if(generate_rest_client)
    target_link_libraries(${name}_idl PUBLIC streaming_http_client rest json streaming)
  endif()

  # Link protobuf library if building protobuf support
  if(generate_protobuf)
    if(TARGET protobuf::libprotobuf)
      target_link_libraries(${name}_idl PUBLIC protobuf::libprotobuf)
      target_include_directories(${name}_idl SYSTEM PRIVATE ${proto_dir} ${output_path}/src)
      # Link the protobuf generated object library if it exists
      if(TARGET ${name}_protobuf_generated)
        target_link_libraries(${name}_idl PRIVATE ${name}_protobuf_generated)
      endif()
    endif()
  endif()
  if(generate_nanopb)
    target_include_directories(${name}_idl SYSTEM PRIVATE ${proto_dir} ${output_path}/src ${nanopb_include_roots})
    if(TARGET ${name}_nanopb_generated)
      target_link_libraries(${name}_idl PUBLIC ${name}_nanopb_generated)
    endif()
    if(TARGET canopy_nanopb_runtime)
      target_link_libraries(${name}_idl PUBLIC canopy_nanopb_runtime)
    endif()
  endif()
  if(generate_protobuf OR generate_nanopb)
    add_dependencies(${name}_idl ${name}_idl_generate)
    if(generate_protobuf)
      add_dependencies(${name}_idl ${name}_proto_compile)
    endif()
    if(generate_nanopb)
      add_dependencies(${name}_idl ${name}_nanopb_compile)
    endif()
  else()
    add_dependencies(${name}_idl ${name}_idl_generate)
  endif()

  foreach(dep ${params_dependencies})
    if(TARGET ${dep}_generate)
      add_dependencies(${name}_idl ${dep}_generate)
    else()
      message("target ${dep}_generate does not exist so skipped")
    endif()

    target_link_libraries(${name}_idl PUBLIC ${dep})
    if(TARGET ${name}_protobuf_generated)
      target_link_libraries(${name}_protobuf_generated PRIVATE ${dep})
    endif()
    if(TARGET ${name}_nanopb_generated)
      target_link_libraries(${name}_nanopb_generated PRIVATE ${dep})
    endif()
  endforeach()

  foreach(dep ${params_link_libraries})
    if(TARGET ${dep})
      target_link_libraries(${name}_idl PRIVATE ${dep})
    else()
      message("target ${dep} does not exist so skipped")
    endif()
  endforeach()

  foreach(dep ${params_dependencies})
    if(${CANOPY_VERBOSE_GENERATOR})
      message("dep ${dep}")
    endif()
    if(TARGET ${dep}_generate)
      add_dependencies(${name}_idl_generate ${dep}_generate)
    else()
      message("target ${dep}_generate does not exist so skipped")
    endif()

    # Add protobuf compilation dependencies if protobuf is enabled Strip _idl suffix from dependency name to get the
    # actual target name
    string(REGEX REPLACE "_idl$" "" dep_base_name "${dep}")
    # Only add dependency if the dependency target has protobuf enabled too
    if(generate_protobuf AND TARGET ${dep_base_name}_proto_compile)
      # Make our proto compilation wait for dependency's proto compilation
      add_dependencies(${name}_proto_compile ${dep_base_name}_proto_compile)
      # Make our protobuf C++ compilation wait for dependency's proto compilation
      add_dependencies(${name}_protobuf_generated ${dep_base_name}_proto_compile)
    endif()
    if(generate_nanopb AND TARGET ${dep_base_name}_nanopb_compile)
      add_dependencies(${name}_nanopb_compile ${dep_base_name}_nanopb_compile)
    endif()
  endforeach()

  foreach(dep ${params_link_libraries})
    if(TARGET ${dep})
      add_dependencies(${name}_idl_generate ${dep})
    else()
      message("target ${dep} does not exist so skipped")
    endif()
  endforeach()

endfunction()

function(
  CanopyGenerateRestOpenApi
  name
  idl
  rest_binding
  output_json
  base_dir)
  set(options)
  set(singleValueArgs title version scheme)
  set(multiValueArgs)
  cmake_parse_arguments(
    params
    "${options}"
    "${singleValueArgs}"
    "${multiValueArgs}"
    ${ARGN})

  get_filename_component(_canopy_openapi_exporter "${_CANOPY_GENERATE_CMAKE_DIR}/../tools/canopy_rest_to_openapi.py"
                         ABSOLUTE)
  find_package(
    Python3
    COMPONENTS Interpreter
    REQUIRED)
  get_filename_component(_canopy_openapi_base_dir_abs "${base_dir}" ABSOLUTE)

  cmake_path(IS_RELATIVE idl _canopy_openapi_idl_is_relative)
  if(_canopy_openapi_idl_is_relative)
    cmake_path(
      APPEND
      "${_canopy_openapi_base_dir_abs}"
      ${idl}
      OUTPUT_VARIABLE
      _canopy_openapi_idl_abs)
  else()
    set(_canopy_openapi_idl_abs "${idl}")
  endif()
  get_filename_component(_canopy_openapi_idl_abs "${_canopy_openapi_idl_abs}" ABSOLUTE)

  cmake_path(IS_RELATIVE rest_binding _canopy_openapi_binding_is_relative)
  if(_canopy_openapi_binding_is_relative)
    cmake_path(
      APPEND
      "${_canopy_openapi_base_dir_abs}"
      ${rest_binding}
      OUTPUT_VARIABLE
      _canopy_openapi_binding_abs)
  else()
    set(_canopy_openapi_binding_abs "${rest_binding}")
  endif()
  get_filename_component(_canopy_openapi_binding_abs "${_canopy_openapi_binding_abs}" ABSOLUTE)

  cmake_path(IS_RELATIVE output_json _canopy_openapi_output_is_relative)
  if(_canopy_openapi_output_is_relative)
    cmake_path(
      APPEND
      "${_canopy_openapi_base_dir_abs}"
      ${output_json}
      OUTPUT_VARIABLE
      _canopy_openapi_output_abs)
  else()
    set(_canopy_openapi_output_abs "${output_json}")
  endif()
  get_filename_component(_canopy_openapi_output_abs "${_canopy_openapi_output_abs}" ABSOLUTE)

  if("${params_title}" STREQUAL "")
    get_filename_component(params_title "${_canopy_openapi_idl_abs}" NAME_WE)
  endif()
  if("${params_version}" STREQUAL "")
    set(params_version "1.0.0")
  endif()
  if("${params_scheme}" STREQUAL "")
    set(params_scheme "https")
  endif()

  add_custom_command(
    OUTPUT "${_canopy_openapi_output_abs}"
    COMMAND
      ${Python3_EXECUTABLE} "${_canopy_openapi_exporter}" --idl "${_canopy_openapi_idl_abs}" --binding
      "${_canopy_openapi_binding_abs}" --output "${_canopy_openapi_output_abs}" --title "${params_title}" --version
      "${params_version}" --scheme "${params_scheme}"
    MAIN_DEPENDENCY "${_canopy_openapi_idl_abs}"
    DEPENDS "${_canopy_openapi_binding_abs}" "${_canopy_openapi_exporter}"
    COMMENT "Generating OpenAPI ${_canopy_openapi_output_abs}")
  add_custom_target(${name} DEPENDS "${_canopy_openapi_output_abs}")
endfunction()
