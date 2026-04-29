#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

enable_language(C)

add_subdirectory(idlparser)

if(CANOPY_BUILD_NANOPB)
  set(CANOPY_NANOPB_RUNTIME_SOURCES
      ${CMAKE_SOURCE_DIR}/c++/submodules/nanopb/pb_common.c
      ${CMAKE_SOURCE_DIR}/c++/submodules/nanopb/pb_decode.c
      ${CMAKE_SOURCE_DIR}/c++/submodules/nanopb/pb_encode.c)

  add_library(
    canopy_nanopb_runtime STATIC
    ${CANOPY_NANOPB_RUNTIME_SOURCES})
  target_include_directories(
    canopy_nanopb_runtime SYSTEM PUBLIC "$<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/c++/submodules/nanopb>")
  set_property(TARGET canopy_nanopb_runtime PROPERTY POSITION_INDEPENDENT_CODE ON)

endif()

if(CANOPY_BUILD_PROTOCOL_BUFFERS OR CANOPY_BUILD_NANOPB)
  # Protocol Buffers - C++ only
  message("Protocol Buffers - building protoc/protobuf components")
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
