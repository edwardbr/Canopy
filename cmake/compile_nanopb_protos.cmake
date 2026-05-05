# Copyright (c) 2026 Edward Boggis-Rolfe All rights reserved.

if(NOT PROTO_DIR)
  message(FATAL_ERROR "PROTO_DIR not specified")
endif()

if(NOT OUTPUT_DIR)
  message(FATAL_ERROR "OUTPUT_DIR not specified")
endif()

if(NOT CPP_OUT_DIR)
  message(FATAL_ERROR "CPP_OUT_DIR not specified")
endif()

if(NOT NANOPB_GENERATOR)
  message(FATAL_ERROR "NANOPB_GENERATOR not specified")
endif()

if(NOT PROTOC)
  message(FATAL_ERROR "PROTOC not specified")
endif()

set(MANIFEST_FILE "${PROTO_DIR}/manifest.txt")
if(NOT EXISTS "${MANIFEST_FILE}")
  message(FATAL_ERROR "Manifest file not found: ${MANIFEST_FILE}")
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

file(MAKE_DIRECTORY "${CPP_OUT_DIR}")

get_filename_component(PROTO_PARENT_DIR "${PROTO_DIR}" DIRECTORY)
get_filename_component(PROTO_SRC_DIR "${PROTO_PARENT_DIR}" DIRECTORY)

set(PROTOC_WRAPPER_DIR "${CPP_OUT_DIR}/.nanopb_tools")
file(MAKE_DIRECTORY "${PROTOC_WRAPPER_DIR}")
file(
  CREATE_LINK
  "${PROTOC}"
  "${PROTOC_WRAPPER_DIR}/protoc"
  SYMBOLIC
  COPY_ON_ERROR)

file(READ "${MANIFEST_FILE}" MANIFEST_CONTENT)
string(REGEX REPLACE "\n" ";" PROTO_FILE_NAMES "${MANIFEST_CONTENT}")

set(PROTOBUF_PYTHONPATH "")
if(PROTOBUF_PYTHON_SOURCE_DIR AND PROTOBUF_SOURCE_PROTO_DIR)
  if(NOT PROTOBUF_PYTHON_OUT_DIR)
    set(PROTOBUF_PYTHON_OUT_DIR "${CPP_OUT_DIR}/.protobuf_python")
  endif()

  set(PROTOBUF_DESCRIPTOR_PROTO "${PROTOBUF_SOURCE_PROTO_DIR}/google/protobuf/descriptor.proto")
  set(PROTOBUF_PLUGIN_PROTO "${PROTOBUF_SOURCE_PROTO_DIR}/google/protobuf/compiler/plugin.proto")
  if(NOT EXISTS "${PROTOBUF_DESCRIPTOR_PROTO}")
    message(FATAL_ERROR "protobuf descriptor.proto not found: ${PROTOBUF_DESCRIPTOR_PROTO}")
  endif()
  if(NOT EXISTS "${PROTOBUF_PLUGIN_PROTO}")
    message(FATAL_ERROR "protobuf plugin.proto not found: ${PROTOBUF_PLUGIN_PROTO}")
  endif()

  file(MAKE_DIRECTORY "${PROTOBUF_PYTHON_OUT_DIR}/google/protobuf/compiler")
  file(MAKE_DIRECTORY "${PROTOBUF_PYTHON_OUT_DIR}/google/protobuf/internal")
  set(PROTOBUF_NAMESPACE_INIT "from pkgutil import extend_path\n__path__ = extend_path(__path__, __name__)\n")
  file(WRITE "${PROTOBUF_PYTHON_OUT_DIR}/google/__init__.py" "${PROTOBUF_NAMESPACE_INIT}")
  file(WRITE "${PROTOBUF_PYTHON_OUT_DIR}/google/protobuf/compiler/__init__.py" "${PROTOBUF_NAMESPACE_INIT}")
  file(WRITE "${PROTOBUF_PYTHON_OUT_DIR}/google/protobuf/internal/__init__.py" "${PROTOBUF_NAMESPACE_INIT}")

  if(EXISTS "${PROTOBUF_PYTHON_SOURCE_DIR}/google/protobuf/__init__.py")
    file(READ "${PROTOBUF_PYTHON_SOURCE_DIR}/google/protobuf/__init__.py" PROTOBUF_PACKAGE_INIT)
  else()
    set(PROTOBUF_PACKAGE_INIT "")
  endif()
  string(APPEND PROTOBUF_PACKAGE_INIT "\n${PROTOBUF_NAMESPACE_INIT}")
  file(WRITE "${PROTOBUF_PYTHON_OUT_DIR}/google/protobuf/__init__.py" "${PROTOBUF_PACKAGE_INIT}")

  execute_process(
    COMMAND "${PROTOC}" --python_out=${PROTOBUF_PYTHON_OUT_DIR} -I "${PROTOBUF_SOURCE_PROTO_DIR}"
            "${PROTOBUF_DESCRIPTOR_PROTO}" "${PROTOBUF_PLUGIN_PROTO}"
    RESULT_VARIABLE PROTOBUF_PYTHON_RESULT
    OUTPUT_VARIABLE PROTOBUF_PYTHON_OUTPUT
    ERROR_VARIABLE PROTOBUF_PYTHON_ERROR)

  if(NOT PROTOBUF_PYTHON_RESULT EQUAL 0)
    message(
      FATAL_ERROR "protobuf Python descriptor generation failed:\n${PROTOBUF_PYTHON_OUTPUT}\n${PROTOBUF_PYTHON_ERROR}")
  endif()

  set(PROTOBUF_EDITION_DEFAULTS_BIN "${PROTOBUF_PYTHON_OUT_DIR}/google/protobuf/internal/python_edition_defaults.binpb")
  execute_process(
    COMMAND "${PROTOC}" --edition_defaults_out=${PROTOBUF_EDITION_DEFAULTS_BIN} --edition_defaults_minimum=PROTO2
            --edition_defaults_maximum=2024 -I "${PROTOBUF_SOURCE_PROTO_DIR}" "${PROTOBUF_DESCRIPTOR_PROTO}"
    RESULT_VARIABLE PROTOBUF_EDITION_DEFAULTS_RESULT
    OUTPUT_VARIABLE PROTOBUF_EDITION_DEFAULTS_OUTPUT
    ERROR_VARIABLE PROTOBUF_EDITION_DEFAULTS_ERROR)

  if(NOT PROTOBUF_EDITION_DEFAULTS_RESULT EQUAL 0)
    message(
      FATAL_ERROR
        "protobuf Python edition defaults generation failed:\n${PROTOBUF_EDITION_DEFAULTS_OUTPUT}\n${PROTOBUF_EDITION_DEFAULTS_ERROR}"
    )
  endif()

  file(READ "${PROTOBUF_EDITION_DEFAULTS_BIN}" PROTOBUF_EDITION_DEFAULTS_HEX HEX)
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "\\\\x\\1" PROTOBUF_EDITION_DEFAULTS_ESCAPED
                       "${PROTOBUF_EDITION_DEFAULTS_HEX}")
  set(PROTOBUF_EDITION_DEFAULTS_TEMPLATE
      "${PROTOBUF_PYTHON_SOURCE_DIR}/google/protobuf/internal/python_edition_defaults.py.template")
  if(EXISTS "${PROTOBUF_EDITION_DEFAULTS_TEMPLATE}")
    file(READ "${PROTOBUF_EDITION_DEFAULTS_TEMPLATE}" PROTOBUF_EDITION_DEFAULTS_PY)
    string(REPLACE "DEFAULTS_VALUE" "${PROTOBUF_EDITION_DEFAULTS_ESCAPED}" PROTOBUF_EDITION_DEFAULTS_PY
                   "${PROTOBUF_EDITION_DEFAULTS_PY}")
  else()
    set(PROTOBUF_EDITION_DEFAULTS_PY
        "_PROTOBUF_INTERNAL_PYTHON_EDITION_DEFAULTS = b\"${PROTOBUF_EDITION_DEFAULTS_ESCAPED}\"\n")
  endif()
  file(WRITE "${PROTOBUF_PYTHON_OUT_DIR}/google/protobuf/internal/python_edition_defaults.py"
       "${PROTOBUF_EDITION_DEFAULTS_PY}")

  set(PROTOBUF_PYTHONPATH "${PROTOBUF_PYTHON_OUT_DIR}:${PROTOBUF_PYTHON_SOURCE_DIR}")
elseif(PROTOBUF_PYTHON_DIR)
  if(EXISTS "${PROTOBUF_PYTHON_DIR}/google/protobuf/descriptor_pb2.py")
    set(PROTOBUF_PYTHONPATH "${PROTOBUF_PYTHON_DIR}")
  else()
    message(FATAL_ERROR "Incomplete protobuf Python tree '${PROTOBUF_PYTHON_DIR}': descriptor_pb2.py is missing.")
  endif()
endif()

set(NANOPB_ENV "PATH=${PROTOC_WRAPPER_DIR}:$ENV{PATH}" "TEMPORARILY_DISABLE_PROTOBUF_VERSION_CHECK=true")
if(PROTOBUF_PYTHONPATH)
  list(APPEND NANOPB_ENV "PYTHONPATH=${PROTOBUF_PYTHONPATH}")
endif()

set(NANOPB_SOURCES "")
foreach(PROTO_NAME ${PROTO_FILE_NAMES})
  string(STRIP "${PROTO_NAME}" PROTO_NAME)
  if(NOT "${PROTO_NAME}" STREQUAL "")
    set(PROTO_FILE "${OUTPUT_DIR}/${PROTO_NAME}")
    string(REGEX REPLACE "\\.proto$" ".pb.c" PB_C_NAME "${PROTO_NAME}")
    list(APPEND NANOPB_SOURCES "${CPP_OUT_DIR}/${PB_C_NAME}")

    execute_process(
      COMMAND ${CMAKE_COMMAND} -E env ${NANOPB_ENV} "${Python3_EXECUTABLE}" "${NANOPB_GENERATOR}" -D "${CPP_OUT_DIR}" -I
              "${OUTPUT_DIR}" "${PROTO_FILE}"
      RESULT_VARIABLE NANOPB_RESULT
      OUTPUT_VARIABLE NANOPB_OUTPUT
      ERROR_VARIABLE NANOPB_ERROR)

    if(NOT NANOPB_RESULT EQUAL 0)
      message(FATAL_ERROR "nanopb_generator failed for ${PROTO_FILE}:\n${NANOPB_OUTPUT}\n${NANOPB_ERROR}")
    endif()
  endif()
endforeach()

if(NOT NANOPB_SOURCES_CMAKE)
  set(NANOPB_SOURCES_CMAKE "${PROTO_DIR}/generated_nanopb_sources.cmake")
endif()

set(NEW_CONTENT "# Generated list of nanopb source files\nset(NANOPB_PB_SOURCES\n")
foreach(pb_c_file ${NANOPB_SOURCES})
  string(APPEND NEW_CONTENT "  \"${pb_c_file}\"\n")
endforeach()
string(APPEND NEW_CONTENT ")\n")

set(SHOULD_WRITE TRUE)
if(EXISTS "${NANOPB_SOURCES_CMAKE}")
  file(READ "${NANOPB_SOURCES_CMAKE}" EXISTING_CONTENT)
  if("${EXISTING_CONTENT}" STREQUAL "${NEW_CONTENT}")
    set(SHOULD_WRITE FALSE)
  endif()
endif()

if(SHOULD_WRITE)
  file(WRITE "${NANOPB_SOURCES_CMAKE}" "${NEW_CONTENT}")
endif()
