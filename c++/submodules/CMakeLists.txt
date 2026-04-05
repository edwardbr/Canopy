#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

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

set(ARGS_BUILD_UNITTESTS OFF)
set(ARGS_BUILD_EXAMPLE OFF)
add_subdirectory(args)
add_library(args::args ALIAS args)

if(CANOPY_BUILD_TEST OR CANOPY_BUILD_GTEST)
  add_subdirectory(googletest)
endif()

add_library(yas_common INTERFACE)

target_include_directories(yas_common SYSTEM INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/yas/include>")

add_subdirectory("${CANOPY_SOURCE_DIR}/submodules/idlparser" idlparser)

add_subdirectory(json)

# spdlog is used by telemetry (CANOPY_USE_TELEMETRY) and test targets (CANOPY_BUILD_TEST)
if(CANOPY_USE_TELEMETRY OR CANOPY_BUILD_TEST)
  set(SPDLOG_FMT_EXTERNAL_HO
      ON
      CACHE BOOL "external fmt header-only" FORCE)
  set(SPDLOG_NO_EXCEPTIONS
      ON
      CACHE BOOL "no exceptions" FORCE)
  set(SPDLOG_INSTALL OFF)
  add_subdirectory(spdlog)
endif()

# json-schema-validator is only used by tests/json_schema_test (requires NLOHMANN_JSON_CONFIG_INSTALL_DIR)
if(DEFINED NLOHMANN_JSON_CONFIG_INSTALL_DIR)
  add_subdirectory(json-schema-validator)
endif()

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
  add_subdirectory("${CANOPY_SOURCE_DIR}/submodules/protobuf" protobuf)
endif()

if(CANOPY_BUILD_COROUTINE)
  set(CARES_STATIC
      $<NOT:${LIBCORO_BUILD_SHARED_LIBS}>
      CACHE INTERNAL "")
  set(CARES_SHARED
      ${LIBCORO_BUILD_SHARED_LIBS}
      CACHE INTERNAL "")
  set(CARES_INSTALL
      OFF
      CACHE INTERNAL "")

  # Enable io_scheduler support in libcoro (required for SPSC transport)
  set(LIBCORO_FEATURE_NETWORKING
      ON
      CACHE BOOL "Enable io_scheduler in libcoro" FORCE)
  set(LIBCORO_BUILD_EXAMPLES
      OFF
      CACHE BOOL "Disable examples" FORCE)

  add_subdirectory(c-ares)
  add_subdirectory(libcoro_for_enclaves)

  if(CANOPY_BUILD_WEBSOCKET)
    add_subdirectory(wslay)

    # llhttp for HTTP parsing - build as simple static library Install dependencies and generate llhttp.c from
    # TypeScript source
    find_program(NODE_EXECUTABLE node)
    find_program(NPM_EXECUTABLE npm)

    if(NOT NODE_EXECUTABLE)
      message(FATAL_ERROR "node executable not found")
    endif()

    add_custom_target(
      llhttp_install_deps
      COMMAND ${CMAKE_COMMAND} -E env NPM_CONFIG_LOGLEVEL=error ${NPM_EXECUTABLE} install --ignore-scripts
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/llhttp
      COMMENT "Installing llhttp dependencies")

    add_custom_command(
      OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/llhttp/build/c/llhttp.c ${CMAKE_CURRENT_SOURCE_DIR}/llhttp/build/llhttp.h
      COMMAND ${CMAKE_COMMAND} -E env NODE_PATH=${CMAKE_CURRENT_SOURCE_DIR}/llhttp/node_modules ${NODE_EXECUTABLE}
              --import tsx bin/generate.ts
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/llhttp
      DEPENDS llhttp_install_deps
      COMMENT "Generating llhttp.c and llhttp.h")

    add_library(
      llhttp_static STATIC
      ${CMAKE_CURRENT_SOURCE_DIR}/llhttp/build/c/llhttp.c ${CMAKE_CURRENT_SOURCE_DIR}/llhttp/src/native/api.c
      ${CMAKE_CURRENT_SOURCE_DIR}/llhttp/src/native/http.c)
    add_custom_target(llhttp_generate DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/llhttp/build/c/llhttp.c
                                              ${CMAKE_CURRENT_SOURCE_DIR}/llhttp/build/llhttp.h)
    add_dependencies(llhttp_static llhttp_generate)
    target_include_directories(llhttp_static PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/llhttp/build
                                                    ${CMAKE_CURRENT_SOURCE_DIR}/llhttp/src/native)
  endif()

  if(CANOPY_BUILD_DEMOS)
    # llama.cpp has update=none in .gitmodules so it is not cloned by default. cmake/Canopy.cmake selectively
    # initialises it when CANOPY_BUILD_DEMOS is ON via CANOPY_REQUIRED_SUBMODULES.
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/llama.cpp/CMakeLists.txt")
      message(
        FATAL_ERROR
          "llama.cpp submodule is not initialised. Run:\n" "  git submodule update --init c++/submodules/llama.cpp\n"
          "or reconfigure with -DCANOPY_BUILD_DEMOS=OFF to skip it.")
    endif()
    set(LLAMA_BUILD_COMMON ON)
    # set(GGML_CUDA ON) set(CMAKE_CUDA_ARCHITECTURES 61) set(GGML_CUDA_F16 OFF) set(CMAKE_CUDA_COMPILER
    # /usr/local/cuda/bin/nvcc) set(CMAKE_CUDA_FLAGS "-allow-unsupported-compiler")
    add_subdirectory(llama.cpp)
  endif()
endif()
