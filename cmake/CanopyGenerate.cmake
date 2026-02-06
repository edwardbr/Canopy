#[[
   Copyright (c) 2024 Edward Boggis-Rolfe
   All rights reserved.
]]

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
      protocol_buffers)
  set(singleValueArgs mock install_dir)
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

  # Define cache variables for global settings with defaults These allow users to override settings while providing
  # sensible defaults
  if(NOT DEFINED CANOPY_BUILD_ENCLAVE)
    set(CANOPY_BUILD_ENCLAVE
        OFF
        CACHE BOOL "Build enclave targets")
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
  if(NOT DEFINED SGX_LIBRARY_PATH)
    set(SGX_LIBRARY_PATH
        ""
        CACHE STRING "SGX library path")
  endif()
  if(NOT DEFINED CANOPY_FMT_LIB)
    set(CANOPY_FMT_LIB
        ""
        CACHE STRING "Host fmt library")
  endif()
  if(NOT DEFINED CANOPY_ENCLAVE_DEFINES)
    set(CANOPY_ENCLAVE_DEFINES
        ""
        CACHE STRING "Enclave compile definitions")
  endif()
  if(NOT DEFINED CANOPY_ENCLAVE_LIBCXX_INCLUDES)
    set(CANOPY_ENCLAVE_LIBCXX_INCLUDES
        ""
        CACHE STRING "Enclave libcxx include directories")
  endif()
  if(NOT DEFINED CANOPY_ENCLAVE_COMPILE_OPTIONS)
    set(CANOPY_ENCLAVE_COMPILE_OPTIONS
        ""
        CACHE STRING "Enclave compile options")
  endif()
  if(NOT DEFINED CANOPY_ENCLAVE_FMT_LIB)
    set(CANOPY_ENCLAVE_FMT_LIB
        ""
        CACHE STRING "Enclave fmt library")
  endif()
  if(NOT DEFINED WARN_OK)
    set(WARN_OK
        ""
        CACHE STRING "Warning flags that are acceptable")
  endif()

  # Extract directory and base filename from IDL path BEFORE converting to absolute idl parameter is like
  # "example_shared/example_shared.idl" or "rpc/rpc_types.idl" or just "example.idl"
  get_filename_component(idl_dir ${idl} DIRECTORY)
  get_filename_component(idl_basename ${idl} NAME_WE)

  # The subdirectory is extracted from the IDL path If idl has no directory (e.g., "example.idl"), use empty
  # subdirectory
  if("${idl_dir}" STREQUAL "")
    set(sub_directory ".")
  else()
    set(sub_directory ${idl_dir})
  endif()

  # The base filename comes from the IDL filename (without .idl extension)
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

  # Construct individual paths for CMake dependency tracking
  set(header_path ${sub_directory}/${base_filename}.h)
  set(proxy_path ${sub_directory}/${base_filename}_proxy.cpp)
  set(stub_path ${sub_directory}/${base_filename}_stub.cpp)
  set(stub_header_path ${sub_directory}/${base_filename}_stub.h)
  set(full_header_path ${output_path}/include/${header_path})
  set(full_proxy_path ${output_path}/src/${proxy_path})
  set(full_stub_path ${output_path}/src/${stub_path})
  set(full_stub_header_path ${output_path}/include/${stub_header_path})
  # Determine which serialization formats to generate
  set(generate_yas FALSE)
  set(generate_protobuf FALSE)

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
    set(generate_protobuf TRUE)
    set(protobuf_path ${sub_directory}/protobuf/${base_filename}.proto)
    set(full_protobuf_path ${output_path}/src/${protobuf_path})
    set(protobuf_cpp_path ${sub_directory}/protobuf/${base_filename}.cpp)
    set(full_protobuf_cpp_path ${output_path}/src/${protobuf_cpp_path})
    set(protobuf_manifest_path ${sub_directory}/protobuf/manifest.txt)
    set(full_protobuf_manifest_path ${output_path}/src/${protobuf_manifest_path})
  else()
    set(protobuf_path "")
    set(full_protobuf_path "")
    set(protobuf_cpp_path "")
    set(full_protobuf_cpp_path "")
    set(protobuf_manifest_path "")
    set(full_protobuf_manifest_path "")
  endif()

  if(${CANOPY_DEBUG_GEN})
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
    message("proxy_path ${proxy_path}")
    message("stub_path ${stub_path}")
    message("stub_header_path ${stub_header_path}")
    message("full_header_path ${full_header_path}")
    message("full_proxy_path ${full_proxy_path}")
    message("full_stub_path ${full_stub_path}")
    message("full_stub_header_path ${full_stub_header_path}")
    message("yas_path ${yas_path}")
    message("full_yas_path ${full_yas_path}")
    message("protobuf_path ${protobuf_path}")
    message("full_protobuf_path ${full_protobuf_path}")
    message("protobuf_cpp_path ${protobuf_cpp_path}")
    message("full_protobuf_cpp_path ${full_protobuf_cpp_path}")
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
  set(ADDITIONAL_HEADERS "")
  set(RETHROW_STUB_EXCEPTION "")
  set(ADDITIONAL_STUB_HEADER "")
  set(GENERATED_DEPENDENCIES "")

  foreach(path ${params_include_paths})
    set(PATHS_PARAMS ${PATHS_PARAMS} --path "${path}")
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
        set(PATHS_PARAMS ${PATHS_PARAMS} --path "${dep_base_dir}")
      endif()

      set(GENERATED_DEPENDENCIES ${GENERATED_DEPENDENCIES} ${dep}_generate)
    else()
      message("target ${dep}_generate does not exist so skipped")
    endif()
    # when installed (used through a package) idl dependencies can be found through their * (or *_enclave) targets: we
    # know that <package_dir>/${param_install_dir}/interfaces/include is in the target's include directories, and that
    # the idls themselves are in <package_dir>/${param_install_dir}
    if(TARGET ${dep})
      get_target_property(include_dirs ${dep} INTERFACE_INCLUDE_DIRECTORIES)
      foreach(include_dir ${include_dirs})
        if(${include_dir} MATCHES "/interfaces/include$")
          string(REPLACE "/interfaces/include" "" idl_dir ${include_dir})
          set(PATHS_PARAMS ${PATHS_PARAMS} --path ${idl_dir})
        endif()
      endforeach()
    endif()
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

  # Build the list of output files based on enabled formats
  set(GENERATOR_OUTPUTS ${full_header_path} ${full_proxy_path} ${full_stub_header_path} ${full_stub_path})

  if(generate_yas)
    list(APPEND GENERATOR_OUTPUTS ${full_yas_path})
  endif()

  if(generate_protobuf)
    # Only the manifest and wrapper C++ file are direct generator outputs Individual .proto files are listed in
    # manifest.txt and compiled separately
    list(APPEND GENERATOR_OUTPUTS ${full_protobuf_cpp_path} ${full_protobuf_manifest_path})
  endif()

  # Build generator command with conditional serialization flags
  set(SERIALIZATION_FLAGS "")
  if(generate_yas)
    set(SERIALIZATION_FLAGS ${SERIALIZATION_FLAGS} --yas)
  endif()

  if(generate_protobuf)
    set(SERIALIZATION_FLAGS ${SERIALIZATION_FLAGS} --protobuf)
  endif()

  # Determine generator dependency for custom command If generator target exists (building from source), depend on the
  # executable file Otherwise use the cached executable path (installed version)
  set(GENERATOR_DEPENDENCY "")
  if(TARGET generator)
    set(GENERATOR_DEPENDENCY $<TARGET_FILE:generator>)
  endif()

  message(
    "  add_custom_command(
    OUTPUT ${GENERATOR_OUTPUTS}
    COMMAND
    ${IDL_GENERATOR} --idl ${idl} --output_path ${output_path} --name ${base_name}
    ${SERIALIZATION_FLAGS} ${PATHS_PARAMS} ${ADDITIONAL_HEADERS}
    ${RETHROW_STUB_EXCEPTION} ${ADDITIONAL_STUB_HEADER}
    MAIN_DEPENDENCY ${idl}
    IMPLICIT_DEPENDS ${idl}
    DEPENDS ${GENERATED_DEPENDENCIES} ${GENERATOR_DEPENDENCY})")

  add_custom_command(
    OUTPUT ${GENERATOR_OUTPUTS}
    COMMAND ${IDL_GENERATOR} --idl ${idl} --output_path ${output_path} --name ${base_name} ${SERIALIZATION_FLAGS}
            ${PATHS_PARAMS} ${ADDITIONAL_HEADERS} ${RETHROW_STUB_EXCEPTION} ${ADDITIONAL_STUB_HEADER}
    MAIN_DEPENDENCY ${idl}
    IMPLICIT_DEPENDS ${idl}
    DEPENDS ${GENERATED_DEPENDENCIES} ${GENERATOR_DEPENDENCY}
    COMMENT "Running generator ${idl}")

  if(${CANOPY_DEBUG_GEN})
    message(
      "
    ${IDL_GENERATOR} --idl ${idl} --output_path ${output_path} --name ${base_name}
    ${SERIALIZATION_FLAGS} ${PATHS_PARAMS} ${ADDITIONAL_HEADERS}
    ${RETHROW_STUB_EXCEPTION} ${ADDITIONAL_STUB_HEADER}
  ")
  endif()

  add_custom_target(${name}_idl_generate DEPENDS ${GENERATOR_OUTPUTS})

  # Ensure generator executable is built before generating IDL
  add_dependencies(${name}_idl_generate generator)

  set_target_properties(${name}_idl_generate PROPERTIES base_dir ${base_dir})

  # Only compile .proto files if protocol_buffers is enabled
  if(generate_protobuf)
    set(proto_dir ${output_path}/src/${sub_directory}/protobuf)
    set(PROTO_MANIFEST "${proto_dir}/manifest.txt")

    # This is a dummy file that ensures CMake has a "source" to track for the library
    set(proto_proxy_src "${CMAKE_CURRENT_BINARY_DIR}/${name}_proto_proxy.cpp")
    if(NOT EXISTS "${proto_proxy_src}")
      file(WRITE "${proto_proxy_src}" "// Generated proxy for ${name}\n")
    endif()

    # The stamp file tracks when the internal compilation script has finished
    set(proto_stamp_file ${proto_dir}/.proto_compiled)

    # Generate a cmake file that lists all the expected .pb.cc files This is created during build after the proto files
    # are compiled
    set(proto_sources_cmake ${proto_dir}/generated_sources.cmake)

    add_custom_command(
      OUTPUT ${proto_stamp_file} ${proto_sources_cmake}
      # Compile the proto files and generate the cmake file with .pb.cc sources
      COMMAND
        ${CMAKE_COMMAND} -D PROTOC=$<TARGET_FILE:protoc> -D SUB_DIR=${sub_directory} -D PROTO_DIR=${proto_dir} -D
        OUTPUT_DIR=${output_path}/src -D PROTO_SOURCES_CMAKE=${proto_sources_cmake} -P
        ${CMAKE_SOURCE_DIR}/cmake/compile_protos.cmake
      COMMAND ${CMAKE_COMMAND} -E touch ${proto_stamp_file}
      DEPENDS ${PROTO_MANIFEST}
      COMMENT "Discovering and compiling .proto files for ${name}")

    add_custom_target(${name}_proto_compile DEPENDS ${proto_stamp_file})

    # Create a placeholder cmake file if it doesn't exist (for configure time)
    if(NOT EXISTS ${proto_sources_cmake})
      file(WRITE ${proto_sources_cmake}
           "# Placeholder - will be regenerated at build time\nset(PROTO_PB_SOURCES \"\")\n")
    endif()

    # Read the manifest.txt file to determine which .pb.cc files will be generated The manifest.txt is created by the
    # IDL generator, so it exists after code generation
    set(PROTO_PB_SOURCES "")
    if(EXISTS ${PROTO_MANIFEST})

      # Read manifest.txt and compute corresponding .pb.cc filenames
      file(READ "${PROTO_MANIFEST}" MANIFEST_CONTENT)
      string(REGEX REPLACE "\n" ";" PROTO_FILE_NAMES "${MANIFEST_CONTENT}")

      foreach(PROTO_NAME ${PROTO_FILE_NAMES})
        string(STRIP "${PROTO_NAME}" PROTO_NAME)
        if(NOT "${PROTO_NAME}" STREQUAL "")
          # Convert example.proto -> example.pb.cc
          string(REGEX REPLACE "\\.proto$" ".pb.cc" PB_CC_NAME "${PROTO_NAME}")
          message("set(pb_cc_file ${output_path}/src / ${PB_CC_NAME})")
          set(pb_cc_file "${output_path}/src/${PB_CC_NAME}")
          list(APPEND PROTO_PB_SOURCES "${pb_cc_file}")
          # Mark as GENERATED so CMake doesn't complain that it doesn't exist yet
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

    # Mark wrapper files as GENERATED
    set_source_files_properties("${full_protobuf_cpp_path}" PROPERTIES GENERATED TRUE)
    set_source_files_properties("${proto_proxy_src}" PROPERTIES GENERATED TRUE)

    # Define the library using the known wrapper, proxy, and all generated .pb.cc files
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

    # CRITICAL: We tell the library to include the object files found in the manifest This uses a generator expression
    # to pull in the objects compiled by our script
    target_link_libraries(${name}_protobuf_generated PRIVATE rpc::rpc)
    # Add protobuf include directories for compilation
    if(TARGET protobuf::libprotobuf)
      target_link_libraries(${name}_protobuf_generated PRIVATE protobuf::libprotobuf)
    endif()
    # Also add the generated protobuf directory for the .pb.h files
    target_include_directories(${name}_protobuf_generated PRIVATE ${proto_dir} ${output_path}/src)
    add_dependencies(${name}_protobuf_generated ${name}_proto_compile ${name}_idl_generate)
  endif()

  # Build library sources based on enabled formats
  set(IDL_SOURCES ${full_header_path} ${full_stub_header_path} ${full_stub_path} ${full_proxy_path})

  if(generate_yas)
    list(APPEND IDL_SOURCES ${full_yas_path})
  endif()

  if(generate_protobuf)
    list(APPEND IDL_SOURCES ${full_protobuf_cpp_path} ${PROTO_PB_SOURCES})
  endif()

  # Create the host IDL library
  add_library(${name}_idl STATIC ${IDL_SOURCES})
  target_compile_definitions(${name}_idl PRIVATE ${CANOPY_DEFINES})
  target_include_directories(
    ${name}_idl
    PUBLIC "$<BUILD_INTERFACE:${output_path}>" "$<BUILD_INTERFACE:${output_path}/include>"
    PRIVATE "${output_path}/include" ${CANOPY_INCLUDES} ${params_include_paths})
  target_compile_options(${name}_idl PRIVATE ${CANOPY_COMPILE_OPTIONS} ${WARN_OK})
  target_link_directories(${name}_idl PUBLIC ${SGX_LIBRARY_PATH})
  set_property(TARGET ${name}_idl PROPERTY COMPILE_PDB_NAME ${name}_idl)

  target_link_libraries(${name}_idl PUBLIC rpc::rpc ${CANOPY_FMT_LIB})

  # Link YAS if any YAS format is enabled
  if(generate_yas)
    target_link_libraries(${name}_idl PUBLIC yas_common)
  endif()

  # Link protobuf library if building protobuf support
  if(generate_protobuf)
    if(TARGET protobuf::libprotobuf)
      target_link_libraries(${name}_idl PUBLIC protobuf::libprotobuf)
      target_include_directories(${name}_idl PRIVATE ${proto_dir} ${output_path}/src)
      # Link the protobuf generated object library if it exists
      if(TARGET ${name}_protobuf_generated)
        target_link_libraries(${name}_idl PRIVATE ${name}_protobuf_generated)
      endif()
    endif()
    add_dependencies(${name}_idl ${name}_idl_generate ${name}_proto_compile)
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
  endforeach()

  foreach(dep ${params_link_libraries})
    if(TARGET ${dep})
      target_link_libraries(${name}_idl PRIVATE ${dep})
    else()
      message("target ${dep} does not exist so skipped")
    endif()
  endforeach()

  if(CANOPY_BUILD_ENCLAVE)
    # Create the enclave IDL library
    add_library(${name}_idl_enclave STATIC ${IDL_SOURCES})
    target_compile_definitions(${name}_idl_enclave PRIVATE ${CANOPY_ENCLAVE_DEFINES})
    target_include_directories(
      ${name}_idl_enclave
      PUBLIC "$<BUILD_INTERFACE:${output_path}>" "$<BUILD_INTERFACE:${output_path}/include>"
      PRIVATE "${output_path}/include" ${CANOPY_ENCLAVE_LIBCXX_INCLUDES} ${params_include_paths})

    target_compile_options(${name}_idl_enclave PRIVATE ${CANOPY_ENCLAVE_COMPILE_OPTIONS} ${WARN_OK})
    target_link_directories(${name}_idl_enclave PRIVATE ${SGX_LIBRARY_PATH})
    set_property(TARGET ${name}_idl_enclave PROPERTY COMPILE_PDB_NAME ${name}_idl_enclave)

    target_link_libraries(${name}_idl_enclave PUBLIC rpc::rpc_enclave ${CANOPY_ENCLAVE_FMT_LIB})

    # Link YAS if any YAS format is enabled
    if(generate_yas)
      target_link_libraries(${name}_idl_enclave PUBLIC yas_common)
    endif()

    # Link protobuf library if building protobuf support
    if(generate_protobuf)
      if(TARGET protobuf::libprotobuf)
        target_link_libraries(${name}_idl_enclave PUBLIC protobuf::libprotobuf)
        target_include_directories(${name}_idl_enclave PRIVATE ${proto_dir} ${output_path}/src)
        # Link the protobuf generated object library if it exists
        if(TARGET ${name}_protobuf_generated)
          target_link_libraries(${name}_idl_enclave PRIVATE ${name}_protobuf_generated)
        endif()
      endif()
      add_dependencies(${name}_idl_enclave ${name}_idl_generate ${name}_proto_compile)
    else()
      add_dependencies(${name}_idl_enclave ${name}_idl_generate)
    endif()

    foreach(dep ${params_dependencies})
      if(TARGET ${dep}_generate)
        add_dependencies(${name}_idl_enclave ${dep}_generate)
      else()
        message("target ${dep}_generate does not exist so skipped")
      endif()

      target_link_libraries(${name}_idl_enclave PUBLIC ${dep}_enclave)
    endforeach()

    foreach(dep ${params_link_libraries})
      if(TARGET ${dep}_enclave)
        target_link_libraries(${name}_idl_enclave PRIVATE ${dep}_enclave)
      else()
        message("target ${dep}_enclave does not exist so skipped")
      endif()
    endforeach()
  endif()

  foreach(dep ${params_dependencies})
    if(${CANOPY_DEBUG_GEN})
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
  endforeach()

  foreach(dep ${params_link_libraries})
    if(TARGET ${dep})
      add_dependencies(${name}_idl_generate ${dep})
    else()
      message("target ${dep} does not exist so skipped")
    endif()
  endforeach()

endfunction()
