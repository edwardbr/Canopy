# Copyright (c) 2026 Edward Boggis-Rolfe All rights reserved.

# This script discovers and compiles all .proto files in a directory It also generates a cmake file listing all expected
# .pb.cc files Usage: - Compile and generate cmake file: cmake -D PROTOC=<path> -D PROTO_DIR=<dir> -D OUTPUT_DIR=<dir>
# -D PROTO_SOURCES_CMAKE=<output_file> -P compile_protos.cmake - Only generate cmake file (no compilation): cmake -D
# PROTO_DIR=<dir> -D PROTO_SOURCES_CMAKE=<output_file> -P compile_protos.cmake

if(NOT PROTO_DIR)
  message(FATAL_ERROR "PROTO_DIR not specified")
endif()

# Set default OUTPUT_DIR if not specified
if(NOT OUTPUT_DIR)
  set(OUTPUT_DIR ${PROTO_DIR})
endif()

# SUB_DIR

message("OUTPUT_DIR ${OUTPUT_DIR}")

# Read .proto files from the manifest.txt file
set(MANIFEST_FILE "${PROTO_DIR}/manifest.txt")

if(NOT EXISTS "${MANIFEST_FILE}")
  message(FATAL_ERROR "Manifest file not found: ${MANIFEST_FILE}")
endif()

# Read the manifest file to get the list of .proto files
file(READ "${MANIFEST_FILE}" MANIFEST_CONTENT)
string(REGEX REPLACE "\n" ";" PROTO_FILE_NAMES "${MANIFEST_CONTENT}")

# Filter out empty lines and build list of proto files
set(PROTO_FILES "")
set(PROTO_PB_SOURCES "")
foreach(PROTO_NAME ${PROTO_FILE_NAMES})
  string(STRIP "${PROTO_NAME}" PROTO_NAME)
  if(NOT "${PROTO_NAME}" STREQUAL "")

    message("APPEND PROTO_FILES ${OUTPUT_DIR} / ${PROTO_NAME})")

    list(APPEND PROTO_FILES "${OUTPUT_DIR}/${PROTO_NAME}")
    # Convert example.proto -> example.pb.cc
    string(REGEX REPLACE "\\.proto$" ".pb.cc" PB_CC_NAME "${PROTO_NAME}")
    set(pb_cc_file "${OUTPUT_DIR}/${PB_CC_NAME}")
    list(APPEND PROTO_PB_SOURCES "${pb_cc_file}")
    message("list(APPEND PROTO_PB_SOURCES ${pb_cc_file})")
  endif()
endforeach()

if(NOT PROTO_FILES)
  message(STATUS "No .proto files listed in ${MANIFEST_FILE}")
  set(PROTO_SOURCES_CMAKE "${PROTO_DIR}/generated_sources.cmake")
  file(WRITE "${PROTO_SOURCES_CMAKE}" "# No proto files found\nset(PROTO_PB_SOURCES \"\")\n")
  return()
endif()

# Only compile if PROTOC is provided
if(PROTOC)
  list(LENGTH PROTO_FILES PROTO_FILE_COUNT)
  message(STATUS "Generating ${PROTO_FILE_COUNT} .proto files from ${MANIFEST_FILE}")

  # Get the parent directories for imports We need to go up from protobuf/ to src/ to get access to all module
  # directories
  get_filename_component(PROTO_PARENT_DIR "${PROTO_DIR}" DIRECTORY) # goes to example/
  get_filename_component(PROTO_SRC_DIR "${PROTO_PARENT_DIR}" DIRECTORY) # goes to src/

  # Use only the src directory as proto_path since all imports now use full paths
  message(STATUS "Using proto_path: ${PROTO_SRC_DIR}")

  # Compile each .proto file
  foreach(PROTO_FILE ${PROTO_FILES})
    message(STATUS "Compiling ${PROTO_FILE}")

    message(
      "        execute_process(
            COMMAND ${PROTOC}
            --proto_path=${PROTO_SRC_DIR}
            --cpp_out=${OUTPUT_DIR}
            ${PROTO_FILE}
            RESULT_VARIABLE PROTOC_RESULT
            OUTPUT_VARIABLE PROTOC_OUTPUT
            ERROR_VARIABLE PROTOC_ERROR
        )")

    # Run protoc with src directory as the single proto_path All imports in .proto files use full paths relative to src
    # directory
    execute_process(
      COMMAND ${PROTOC} --proto_path=${PROTO_SRC_DIR} --cpp_out=${OUTPUT_DIR} ${PROTO_FILE}
      RESULT_VARIABLE PROTOC_RESULT
      OUTPUT_VARIABLE PROTOC_OUTPUT
      ERROR_VARIABLE PROTOC_ERROR)

    if(NOT PROTOC_RESULT EQUAL 0)
      message(FATAL_ERROR "protoc failed for ${PROTO_FILE}:\n${PROTOC_ERROR}")
    endif()

    get_filename_component(PROTO_NAME "${PROTO_FILE}" NAME_WE)
    message(STATUS "Generated ${PROTO_NAME}.pb.h and ${PROTO_NAME}.pb.cc")
  endforeach()

  message(STATUS "Successfully compiled all .proto files")
else()
  message(STATUS "PROTOC not specified, skipping proto compilation")
endif()

# Generate the cmake file with PROTO_PB_SOURCES Use PROTO_SOURCES_CMAKE if provided, otherwise use default location
if(NOT PROTO_SOURCES_CMAKE)
  set(PROTO_SOURCES_CMAKE "${PROTO_DIR}/generated_sources.cmake")
endif()

# Build the new content
set(NEW_CONTENT "# Generated list of .pb.cc source files\nset(PROTO_PB_SOURCES\n")
foreach(pb_cc_file ${PROTO_PB_SOURCES})
  string(APPEND NEW_CONTENT "  \"${pb_cc_file}\"\n")
endforeach()
string(APPEND NEW_CONTENT ")\n")

# Only write if content has changed (to avoid unnecessary timestamp updates)
set(SHOULD_WRITE TRUE)
if(EXISTS "${PROTO_SOURCES_CMAKE}")
  file(READ "${PROTO_SOURCES_CMAKE}" EXISTING_CONTENT)
  if("${EXISTING_CONTENT}" STREQUAL "${NEW_CONTENT}")
    set(SHOULD_WRITE FALSE)
  endif()
endif()

if(SHOULD_WRITE)
  file(WRITE "${PROTO_SOURCES_CMAKE}" "${NEW_CONTENT}")
  message(STATUS "Generated ${PROTO_SOURCES_CMAKE} with ${CMAKE_LIST_LENGTH} source files")
else()
  message(STATUS "${PROTO_SOURCES_CMAKE} is up-to-date")
endif()
