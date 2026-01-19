#[[
   Copyright (c) 2024 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

if(CANOPY_STANDALONE)
  if(WIN32)
    if(${BUILD_TYPE} STREQUAL "release")
      set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /EHsc /MD")
    else()
      set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /EHsc /MDd")
    endif()
  endif()

  set(FMT_INSTALL ON)
  add_subdirectory(fmt)

  # fmt CMakeLists.txt defines a PUBLIC_HEADER property which triggers a warning when used as a subproject
  set_property(TARGET fmt PROPERTY PUBLIC_HEADER)

  add_subdirectory(args)
  add_library(args::args ALIAS args)
  add_subdirectory(googletest)

  add_library(yas_common INTERFACE)

  target_include_directories(yas_common INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/yas/include>")

  add_subdirectory(idlparser)

  # Removed: CANOPY_BUILD_TEST commented out - use CANOPY_BUILD_TEST instead
  # if(CANOPY_BUILD_TEST)
  message("spdlog this is used in tests only and does not need to be installed for other users")
  set(SPDLOG_FMT_EXTERNAL_HO
    ON
    CACHE BOOL "external fmt header-only" FORCE)
  set(SPDLOG_NO_EXCEPTIONS
    ON
    CACHE BOOL "no exceptions" FORCE)
  set(SPDLOG_INSTALL OFF)
  add_subdirectory(spdlog)

  # endif(CANOPY_BUILD_TEST)
  add_subdirectory(json)
  add_subdirectory(json-schema-validator)

  # Protocol Buffers - C++ only
  message("Protocol Buffers - building C++ components only")
  set(protobuf_INSTALL OFF CACHE BOOL "Install protobuf" FORCE)
  set(protobuf_BUILD_TESTS OFF CACHE BOOL "Build protobuf tests" FORCE)
  set(protobuf_BUILD_CONFORMANCE OFF CACHE BOOL "Build conformance tests" FORCE)
  set(protobuf_BUILD_EXAMPLES OFF CACHE BOOL "Build examples" FORCE)
  set(protobuf_BUILD_PROTOBUF_BINARIES ON CACHE BOOL "Build protobuf libraries and protoc" FORCE)
  set(protobuf_BUILD_PROTOC_BINARIES ON CACHE BOOL "Build protoc compiler" FORCE)
  set(protobuf_BUILD_LIBUPB ON CACHE BOOL "Build libupb (required for protoc)" FORCE)
  add_subdirectory(protobuf)

  if(CANOPY_BUILD_COROUTINE)
    set(CARES_STATIC $<NOT:${LIBCORO_BUILD_SHARED_LIBS}> CACHE INTERNAL "")
    set(CARES_SHARED ${LIBCORO_BUILD_SHARED_LIBS} CACHE INTERNAL "")
    set(CARES_INSTALL OFF CACHE INTERNAL "")

    # Enable io_scheduler support in libcoro (required for SPSC transport)
    set(LIBCORO_FEATURE_NETWORKING ON CACHE BOOL "Enable io_scheduler in libcoro" FORCE)

    # set(c-ares_DIR "${CMAKE_SOURCE_DIR}/submodules/c-ares")
    # list(APPEND CMAKE_PREFIX_PATH "${CMAKE_SOURCE_DIR}/submodules/c-ares")
    add_subdirectory(c-ares)
    add_subdirectory(libcoro)
    add_subdirectory(wslay)

    # llhttp for HTTP parsing - build as simple static library
    # Install dependencies and generate llhttp.c from TypeScript source
    find_program(NODE_EXECUTABLE node)
    find_program(NPM_EXECUTABLE npm)

    if(NOT NODE_EXECUTABLE)
      message(FATAL_ERROR "node executable not found")
    endif()

    add_custom_target(llhttp_install_deps
      COMMAND ${CMAKE_COMMAND} -E env NPM_CONFIG_LOGLEVEL=error
      ${NPM_EXECUTABLE} install --ignore-scripts
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/llhttp
      COMMENT "Installing llhttp dependencies"
    )

    add_custom_command(
      OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/llhttp/build/c/llhttp.c
      ${CMAKE_CURRENT_SOURCE_DIR}/llhttp/build/llhttp.h
      COMMAND ${CMAKE_COMMAND} -E env NODE_PATH=${CMAKE_CURRENT_SOURCE_DIR}/llhttp/node_modules
      ${NODE_EXECUTABLE} --import tsx bin/generate.ts
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/llhttp
      DEPENDS llhttp_install_deps
      COMMENT "Generating llhttp.c and llhttp.h"
    )

    add_library(llhttp_static STATIC
      ${CMAKE_CURRENT_SOURCE_DIR}/llhttp/build/c/llhttp.c
      ${CMAKE_CURRENT_SOURCE_DIR}/llhttp/src/native/api.c
      ${CMAKE_CURRENT_SOURCE_DIR}/llhttp/src/native/http.c
    )
    add_custom_target(llhttp_generate DEPENDS
      ${CMAKE_CURRENT_SOURCE_DIR}/llhttp/build/c/llhttp.c
      ${CMAKE_CURRENT_SOURCE_DIR}/llhttp/build/llhttp.h
    )
    add_dependencies(llhttp_static llhttp_generate)
    target_include_directories(llhttp_static PUBLIC
      ${CMAKE_CURRENT_SOURCE_DIR}/llhttp/build
      ${CMAKE_CURRENT_SOURCE_DIR}/llhttp/src/native
    )
  endif()
endif()
