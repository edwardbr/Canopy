#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

enable_language(C)

add_subdirectory(idlparser)

if(CANOPY_BUILD_NANOPB)
  set(CANOPY_NANOPB_RUNTIME_SOURCES
      ${CANOPY_CPP_SOURCE_DIR}/submodules/nanopb/pb_common.c ${CANOPY_CPP_SOURCE_DIR}/submodules/nanopb/pb_decode.c
      ${CANOPY_CPP_SOURCE_DIR}/submodules/nanopb/pb_encode.c)

  add_library(canopy_nanopb_runtime STATIC ${CANOPY_NANOPB_RUNTIME_SOURCES})
  target_include_directories(canopy_nanopb_runtime SYSTEM
                             PUBLIC "$<BUILD_INTERFACE:${CANOPY_CPP_SOURCE_DIR}/submodules/nanopb>")
  set_property(TARGET canopy_nanopb_runtime PROPERTY POSITION_INDEPENDENT_CODE ON)

  if(CANOPY_BUILD_ENCLAVE)
    add_library(
      canopy_nanopb_runtime_enclave STATIC
      ${CANOPY_NANOPB_RUNTIME_SOURCES})
    target_compile_definitions(canopy_nanopb_runtime_enclave PRIVATE ${CANOPY_ENCLAVE_DEFINES})
    target_compile_options(
      canopy_nanopb_runtime_enclave PRIVATE ${CANOPY_ENCLAVE_COMPILE_OPTIONS} ${CANOPY_WARN_OK_ENCLAVE}
                                             "SHELL:-Wno-unused-command-line-argument")
    target_include_directories(
      canopy_nanopb_runtime_enclave SYSTEM PUBLIC "$<BUILD_INTERFACE:${CANOPY_CPP_SOURCE_DIR}/submodules/nanopb>")
    target_include_directories(canopy_nanopb_runtime_enclave PRIVATE ${CANOPY_ENCLAVE_LIBCXX_INCLUDES})
    set_property(TARGET canopy_nanopb_runtime_enclave PROPERTY POSITION_INDEPENDENT_CODE ON)
  endif()
endif()

set(CANOPY_NEED_BUNDLED_PROTOC OFF)
if(CANOPY_BUILD_NANOPB
   AND NOT CANOPY_BUILD_PROTOCOL_BUFFERS
   AND NOT CANOPY_NANOPB_PROTOC_EXECUTABLE)
  find_program(CANOPY_SYSTEM_PROTOC_EXECUTABLE protoc)
  if(CANOPY_SYSTEM_PROTOC_EXECUTABLE)
    set(CANOPY_NANOPB_PROTOC_EXECUTABLE
        "${CANOPY_SYSTEM_PROTOC_EXECUTABLE}"
        CACHE FILEPATH "protoc executable used by Nanopb generator" FORCE)
  else()
    set(CANOPY_NEED_BUNDLED_PROTOC ON)
  endif()
endif()

if(CANOPY_BUILD_PROTOCOL_BUFFERS OR CANOPY_NEED_BUNDLED_PROTOC)
  if(CANOPY_BUILD_PROTOCOL_BUFFERS)
    message("Protocol Buffers - building protoc/protobuf components")
  else()
    message("Nanopb - no system protoc found; building bundled protoc tool")
  endif()
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
  if(CANOPY_BUILD_PROTOCOL_BUFFERS)
    add_subdirectory(protobuf)
  else()
    add_subdirectory(protobuf EXCLUDE_FROM_ALL)
  endif()
endif()
