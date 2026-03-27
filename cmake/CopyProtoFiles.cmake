#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.

   CopyProtoFiles.cmake — build-time script that reads a protobuf manifest.txt and
   copies each listed .proto file into an output directory, preserving the path
   structure relative to PROTO_SRC_ROOT.

   Called by CanopyJavascriptGenerate-generated custom commands with:
     -D PROTO_DIR=<dir containing manifest.txt and .proto files>
     -D PROTO_SRC_ROOT=<root from which manifest entries are relative>
     -D OUTPUT_DIR=<destination root>
     -D STAMP_FILE=<file to touch on completion>
]]

file(READ "${PROTO_DIR}/manifest.txt" MANIFEST_CONTENT)
string(REGEX REPLACE "\n" ";" PROTO_NAMES "${MANIFEST_CONTENT}")
foreach(PROTO_NAME ${PROTO_NAMES})
  string(STRIP "${PROTO_NAME}" PROTO_NAME)
  if(NOT "${PROTO_NAME}" STREQUAL "")
    get_filename_component(dest_dir "${OUTPUT_DIR}/${PROTO_NAME}" DIRECTORY)
    file(MAKE_DIRECTORY "${dest_dir}")
    file(COPY_FILE "${PROTO_SRC_ROOT}/${PROTO_NAME}" "${OUTPUT_DIR}/${PROTO_NAME}" ONLY_IF_DIFFERENT)
  endif()
endforeach()
file(TOUCH "${STAMP_FILE}")
