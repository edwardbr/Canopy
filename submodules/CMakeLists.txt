#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

add_subdirectory(idlparser)

if(CANOPY_BUILD_PROTOCOL_BUFFERS)
  # Protocol Buffers - C++ only
  message("Protocol Buffers - building C++ components only")
  set(protobuf_INSTALL
      OFF
      CACHE BOOL "Install protobuf" FORCE)
  set(protobuf_BUILD_TESTS
      OFF
      CACHE BOOL "Build protobuf tests" FORCE)
  set(protobuf_BUILD_CONFORMANCE
      OFF
      CACHE BOOL "Build conformance tests" FORCE)
  set(protobuf_BUILD_EXAMPLES
      OFF
      CACHE BOOL "Build examples" FORCE)
  set(protobuf_BUILD_PROTOBUF_BINARIES
      ON
      CACHE BOOL "Build protobuf libraries and protoc" FORCE)
  set(protobuf_BUILD_PROTOC_BINARIES
      ON
      CACHE BOOL "Build protoc compiler" FORCE)
  set(protobuf_BUILD_LIBUPB
      ON
      CACHE BOOL "Build libupb (required for protoc)" FORCE)
  add_subdirectory(protobuf)
endif()
