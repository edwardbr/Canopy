#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.

   CanopyJavascriptGenerate — generates a UMD JavaScript proxy/stub file from an IDL
   directly into the specified output directory, copies all required .proto files from
   IDL dependencies, optionally copies extra hand-written JS files, and optionally runs
   pbjs to compile a protobuf JavaScript module.

   pbjs is run from cmake/pbjs/ — an isolated package that is part of the Canopy build
   infrastructure and has no relationship to any consuming application's package.json.

   Usage:
     CanopyJavascriptGenerate(
       <name>                         # Target name prefix (e.g., "websocket_demo_client")
       <idl>                          # IDL path relative to base_dir
       <base_dir>                     # Root directory for resolving relative IDL path
       <output_dir>                   # Directory to write all generated files into
       [dependencies <targets...>]    # IDL dependency targets (provides --path resolution
                                      #   and proto file discovery)
       [include_paths <paths...>]     # Extra IDL include paths
       [copy_files <paths...>]        # Extra source files to copy into output_dir
       [compile_proto_js]             # Run pbjs to produce <proto_js_name>.js
       [proto_js_format <fmt>]        # "commonjs" adds -w commonjs to pbjs; omit for UMD.
       [proto_js_name <name>]         # Base name for the pbjs output (default: <idl_basename>_proto)
     )

   Produces:
     ${name}_js_generate — ALL custom target that drives:
       • generator --javascript  → ${output_dir}/<idl_basename>.js
       • CopyProtoFiles.cmake    → ${output_dir}/**/*.proto  (one run per IDL dependency)
       • copy_if_different       → ${output_dir}/<filename>  (one per copy_files entry)
       • pbjs                    → ${output_dir}/<proto_js_name>.js  (when compile_proto_js set)

   All stamp files live in CMAKE_CURRENT_BINARY_DIR under the target name prefix so
   multiple calls with different <name>s in the same CMakeLists.txt never collide.
]]

set(_CANOPY_JS_GENERATE_CMAKE_DIR
    "${CMAKE_CURRENT_LIST_DIR}"
    CACHE INTERNAL "")

function(
  CanopyJavascriptGenerate
  name
  idl
  base_dir
  output_dir)
  set(options compile_proto_js)
  set(oneValueArgs proto_js_format proto_js_name)
  set(multiValueArgs dependencies include_paths copy_files)
  cmake_parse_arguments(
    "params"
    "${options}"
    "${oneValueArgs}"
    "${multiValueArgs}"
    ${ARGN})

  # Resolve IDL to absolute path
  get_filename_component(idl_basename ${idl} NAME_WE)
  cmake_path(IS_RELATIVE idl idl_is_relative)
  if(${idl_is_relative})
    cmake_path(
      APPEND
      base_dir
      ${idl}
      OUTPUT_VARIABLE
      idl)
  endif()

  set(_generator_output_dir "${CMAKE_BINARY_DIR}/generated/javascript/${name}")
  set(_generator_js_output "${_generator_output_dir}/${idl_basename}.js")
  set(js_output "${output_dir}/${idl_basename}.js")
  set_source_files_properties("${_generator_js_output}" PROPERTIES GENERATED TRUE)
  set_source_files_properties("${js_output}" PROPERTIES GENERATED TRUE)

  if(NOT DEFINED CANOPY_IDL_GENERATOR_EXECUTABLE)
    set(CANOPY_IDL_GENERATOR_EXECUTABLE
        "generator"
        CACHE STRING "Path to the IDL generator executable")
  endif()
  set(IDL_GENERATOR ${CANOPY_IDL_GENERATOR_EXECUTABLE})

  set(PATHS_PARAMS "")
  set(GENERATED_DEPENDENCIES "")
  set(ALL_STAMPS "")
  set(PROTO_COPY_STAMPS "")

  foreach(path ${params_include_paths})
    set(PATHS_PARAMS ${PATHS_PARAMS} --path "${path}")
  endforeach()

  # ---- Dependency resolution + proto copy helper ---------------------------------
  # Adds a proto-copy stamp command for one IDL generate target and appends the stamp path to ALL_STAMPS. Sets <out_var>
  # to the created stamp path.
  function(_proto_copy_stamp_for gen_target suffix out_var)
    get_target_property(_pd ${gen_target} proto_dir)
    get_target_property(_psr ${gen_target} proto_src_root)
    if(_pd AND _psr)
      set(_s "${CMAKE_CURRENT_BINARY_DIR}/${name}_${suffix}_proto_copy.stamp")
      add_custom_command(
        OUTPUT "${_s}"
        COMMAND ${CMAKE_COMMAND} -D "PROTO_DIR=${_pd}" -D "PROTO_SRC_ROOT=${_psr}" -D "OUTPUT_DIR=${output_dir}" -D
                "STAMP_FILE=${_s}" -P "${_CANOPY_JS_GENERATE_CMAKE_DIR}/CopyProtoFiles.cmake"
        DEPENDS "${_pd}/manifest.txt" ${gen_target}
        COMMENT "Copying proto files for ${suffix} into ${output_dir}")
      set(ALL_STAMPS
          "${ALL_STAMPS}" "${_s}"
          PARENT_SCOPE)
      set(PROTO_COPY_STAMPS
          "${PROTO_COPY_STAMPS}" "${_s}"
          PARENT_SCOPE)
      set(${out_var}
          "${_s}"
          PARENT_SCOPE)
    else()
      set(${out_var}
          ""
          PARENT_SCOPE)
    endif()
  endfunction()

  # Resolve each listed dependency: IDL path + proto copy
  foreach(dep ${params_dependencies})
    if(TARGET ${dep}_generate)
      get_target_property(dep_base_dir ${dep}_generate base_dir)
      if(dep_base_dir)
        set(PATHS_PARAMS ${PATHS_PARAMS} --path "${dep_base_dir}")
      endif()
      set(GENERATED_DEPENDENCIES ${GENERATED_DEPENDENCIES} ${dep}_generate)
      _proto_copy_stamp_for(${dep}_generate ${dep} _ignored)
    else()
      message(STATUS "CanopyJavascriptGenerate: target ${dep}_generate does not exist, skipped")
    endif()
    if(TARGET ${dep})
      get_target_property(include_dirs ${dep} INTERFACE_INCLUDE_DIRECTORIES)
      foreach(include_dir ${include_dirs})
        if(${include_dir} MATCHES "/interfaces/include$")
          string(REPLACE "/interfaces/include" "" idl_dir2 ${include_dir})
          set(PATHS_PARAMS ${PATHS_PARAMS} --path ${idl_dir2})
        endif()
      endforeach()
    endif()
  endforeach()

  # Primary IDL's own proto files (target: <idl_basename>_idl_generate)
  set(_own_proto_stamp "")
  if(TARGET ${idl_basename}_idl_generate)
    _proto_copy_stamp_for(${idl_basename}_idl_generate ${idl_basename} _own_proto_stamp)
  endif()

  # ---- JS generation -------------------------------------------------------------
  set(GENERATOR_DEPENDENCY "")
  if(TARGET generator)
    set(GENERATOR_DEPENDENCY $<TARGET_FILE:generator>)
  endif()

  set(_js_stamp "${CMAKE_CURRENT_BINARY_DIR}/${name}_js_generate.stamp")
  add_custom_command(
    OUTPUT "${_js_stamp}"
    BYPRODUCTS "${_generator_js_output}" "${js_output}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_generator_output_dir}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${output_dir}"
    COMMAND ${IDL_GENERATOR} --idl "${idl}" --output_path "${_generator_output_dir}" --name "${idl_basename}"
            --javascript ${PATHS_PARAMS}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_generator_js_output}" "${js_output}"
    COMMAND ${CMAKE_COMMAND} -E touch "${_js_stamp}"
    MAIN_DEPENDENCY "${idl}"
    DEPENDS ${GENERATED_DEPENDENCIES} ${GENERATOR_DEPENDENCY}
    COMMENT "Generating JavaScript proxy/stub for ${idl_basename} into ${output_dir}")
  list(APPEND ALL_STAMPS "${_js_stamp}")

  # ---- copy_files ----------------------------------------------------------------
  foreach(_src ${params_copy_files})
    get_filename_component(_fname "${_src}" NAME)
    string(MAKE_C_IDENTIFIER "${_fname}" _safe)
    set(_dst "${output_dir}/${_fname}")
    set(_copy_stamp "${CMAKE_CURRENT_BINARY_DIR}/${name}_${_safe}_copy.stamp")
    add_custom_command(
      OUTPUT "${_copy_stamp}"
      BYPRODUCTS "${_dst}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${output_dir}"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_src}" "${_dst}"
      COMMAND ${CMAKE_COMMAND} -E touch "${_copy_stamp}"
      DEPENDS "${_src}"
      COMMENT "Copying ${_fname} to ${output_dir}")
    list(APPEND ALL_STAMPS "${_copy_stamp}")
  endforeach()

  # ---- pbjs protobuf JS compilation ----------------------------------------------
  # pbjs runs from cmake/pbjs/ — an isolated Canopy build-tool package entirely separate from any consuming
  # application's node_modules.
  if(params_compile_proto_js)
    find_program(NPM_EXECUTABLE npm)
    find_program(NODE_EXECUTABLE node)

    if(NPM_EXECUTABLE AND NODE_EXECUTABLE)
      set(_proto_input_root "${CMAKE_BINARY_DIR}/generated/src")
      set(_pbjs_dir "${_CANOPY_JS_GENERATE_CMAKE_DIR}/pbjs")
      set(_pbjs "${_pbjs_dir}/node_modules/.bin/pbjs")
      set(_npm_stamp "${_pbjs_dir}/node_modules/.protobufjs-cli.stamp")

      # The npm install runs once regardless of how many CanopyJavascriptGenerate calls request proto JS compilation —
      # the output path is always the same.
      get_property(_npm_done GLOBAL PROPERTY "CanopyPbjsInstalled")
      if(NOT _npm_done)
        add_custom_command(
          OUTPUT "${_npm_stamp}"
          COMMAND
            ${CMAKE_COMMAND} -DNPM_EXECUTABLE=${NPM_EXECUTABLE} -DCLIENT_DIR=${_pbjs_dir} -DPBJS_EXECUTABLE=${_pbjs}
            -DSTAMP_FILE=${_npm_stamp} -P "${_CANOPY_JS_GENERATE_CMAKE_DIR}/EnsurePbjsInstalled.cmake"
          COMMENT "Installing protobufjs-cli (Canopy build tool)")
        set_property(GLOBAL PROPERTY "CanopyPbjsInstalled" TRUE)
      endif()

      # Determine output name and format flags
      if(params_proto_js_name)
        set(_proto_js_base "${params_proto_js_name}")
      else()
        set(_proto_js_base "${idl_basename}_proto")
      endif()
      set(_proto_js_out "${output_dir}/${_proto_js_base}.js")

      set(_pbjs_flags "")
      if("${params_proto_js_format}" STREQUAL "commonjs")
        list(APPEND _pbjs_flags -w commonjs)
      endif()

      # Entry proto: <idl_basename>/protobuf/<idl_basename>_all.proto (relative to output_dir)
      set(_all_proto "${idl_basename}/protobuf/${idl_basename}_all.proto")

      set(_pbjs_stamp "${CMAKE_CURRENT_BINARY_DIR}/${name}_proto_js.stamp")
      add_custom_command(
        OUTPUT "${_pbjs_stamp}"
        BYPRODUCTS "${_proto_js_out}"
        COMMAND "${_pbjs}" -t static-module ${_pbjs_flags} --path "${_proto_input_root}" -o "${_proto_js_out}"
                "${_all_proto}"
        COMMAND ${CMAKE_COMMAND} -E touch "${_pbjs_stamp}"
        DEPENDS "${_npm_stamp}" ${PROTO_COPY_STAMPS}
        WORKING_DIRECTORY "${_pbjs_dir}"
        COMMENT "Compiling protobuf JS for ${idl_basename} (${_proto_js_base}.js)")
      list(APPEND ALL_STAMPS "${_pbjs_stamp}")
    else()
      if(NOT NPM_EXECUTABLE)
        message(STATUS "CanopyJavascriptGenerate: npm not found — ${name} proto JS skipped")
      elseif(NOT NODE_EXECUTABLE)
        message(STATUS "CanopyJavascriptGenerate: node not found — ${name} proto JS skipped")
      endif()
    endif()
  endif()

  # ---- Final target --------------------------------------------------------------
  add_custom_target(${name}_js_generate ALL DEPENDS ${ALL_STAMPS})
  if(TARGET generator)
    add_dependencies(${name}_js_generate generator)
  endif()
  set_target_properties(${name}_js_generate PROPERTIES base_dir "${base_dir}")
endfunction()
