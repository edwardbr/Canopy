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

set(NANOPB_SOURCES "")
foreach(PROTO_NAME ${PROTO_FILE_NAMES})
  string(STRIP "${PROTO_NAME}" PROTO_NAME)
  if(NOT "${PROTO_NAME}" STREQUAL "")
    set(PROTO_FILE "${OUTPUT_DIR}/${PROTO_NAME}")
    string(REGEX REPLACE "\\.proto$" ".pb.c" PB_C_NAME "${PROTO_NAME}")
    list(APPEND NANOPB_SOURCES "${CPP_OUT_DIR}/${PB_C_NAME}")

    execute_process(
      COMMAND
        ${CMAKE_COMMAND} -E env "PATH=${PROTOC_WRAPPER_DIR}:$ENV{PATH}"
        "TEMPORARILY_DISABLE_PROTOBUF_VERSION_CHECK=true" "${Python3_EXECUTABLE}" "${NANOPB_GENERATOR}" -D
        "${CPP_OUT_DIR}" -I "${OUTPUT_DIR}" "${PROTO_FILE}"
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

if(NANOPB_AGGREGATE_SOURCE)
  set(AGGREGATE_CONTENT "/* Generated aggregate source for nanopb descriptors. */\n")
  foreach(pb_c_file ${NANOPB_SOURCES})
    string(APPEND AGGREGATE_CONTENT "#include \"${pb_c_file}\"\n")
  endforeach()

  set(SHOULD_WRITE_AGGREGATE TRUE)
  if(EXISTS "${NANOPB_AGGREGATE_SOURCE}")
    file(READ "${NANOPB_AGGREGATE_SOURCE}" EXISTING_AGGREGATE_CONTENT)
    if("${EXISTING_AGGREGATE_CONTENT}" STREQUAL "${AGGREGATE_CONTENT}")
      set(SHOULD_WRITE_AGGREGATE FALSE)
    endif()
  endif()

  if(SHOULD_WRITE_AGGREGATE)
    get_filename_component(NANOPB_AGGREGATE_DIR "${NANOPB_AGGREGATE_SOURCE}" DIRECTORY)
    file(MAKE_DIRECTORY "${NANOPB_AGGREGATE_DIR}")
    file(WRITE "${NANOPB_AGGREGATE_SOURCE}" "${AGGREGATE_CONTENT}")
  endif()
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
