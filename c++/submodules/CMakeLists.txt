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
  set(LIBCORO_BUILD_TESTS
      OFF
      CACHE BOOL "Disable libcoro tests" FORCE)
  set(LIBCORO_EXTERNAL_DEPENDENCIES
      ON
      CACHE BOOL "Use Canopy-provided dependencies for libcoro" FORCE)

  add_subdirectory(c-ares)
  add_library(canopy_cares INTERFACE)
  target_link_libraries(canopy_cares INTERFACE c-ares::cares)

  set(CANOPY_CARES_CONFIG_DIR "${CMAKE_CURRENT_BINARY_DIR}/canopy-cares-config")
  file(MAKE_DIRECTORY "${CANOPY_CARES_CONFIG_DIR}")
  file(
    WRITE "${CANOPY_CARES_CONFIG_DIR}/c-ares-config.cmake"
    "if(NOT TARGET c-ares::cares)\n"
    "  message(FATAL_ERROR \"Canopy c-ares target is not available\")\n"
    "endif()\n")
  set(c-ares_DIR
      "${CANOPY_CARES_CONFIG_DIR}"
      CACHE PATH "Canopy c-ares package shim for libcoro" FORCE)

  add_subdirectory(libcoro)

  if(CANOPY_BUILD_ENCLAVE)
    add_library(libcoro_enclave INTERFACE)
    target_compile_features(libcoro_enclave INTERFACE cxx_std_20)
    # Compatibility target for older enclave target link lines. Enclave code should
    # consume the RPC-owned coroutine runtime/polyfill headers rather than libcoro.
    target_include_directories(
      libcoro_enclave
      INTERFACE ${CMAKE_SOURCE_DIR}/c++/rpc/include
                ${CMAKE_SOURCE_DIR}/c++/rpc/include/rpc/internal/polyfill/sgx)
  endif()

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

    if(CANOPY_BUILD_ENCLAVE)
      add_library(
        llhttp_enclave STATIC
        ${CMAKE_CURRENT_SOURCE_DIR}/llhttp/build/c/llhttp.c ${CMAKE_CURRENT_SOURCE_DIR}/llhttp/src/native/api.c
        ${CMAKE_CURRENT_SOURCE_DIR}/llhttp/src/native/http.c)
      add_dependencies(llhttp_enclave llhttp_generate)
      target_include_directories(
        llhttp_enclave
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/llhttp/build ${CMAKE_CURRENT_SOURCE_DIR}/llhttp/src/native
        PRIVATE ${CANOPY_ENCLAVE_LIBCXX_INCLUDES})
      target_compile_definitions(llhttp_enclave PRIVATE ${CANOPY_ENCLAVE_DEFINES})
      target_compile_options(
        llhttp_enclave
        PRIVATE
          ${CANOPY_ENCLAVE_COMPILE_OPTIONS}
          ${CANOPY_WARN_OK_ENCLAVE}
          $<$<COMPILE_LANG_AND_ID:C,Clang>:-Wno-unused-command-line-argument>
          $<$<COMPILE_LANG_AND_ID:C,Clang>:-Wno-implicit-fallthrough>
          -include
          ${CANOPY_CPP_SOURCE_DIR}/subcomponents/http_server/include/canopy/http_server/detail/llhttp_enclave_stdio_compat.h)
      target_link_options(llhttp_enclave PRIVATE ${CANOPY_ENCLAVE_LINK_OPTIONS})

      set(CANOPY_WSLAY_ENCLAVE_CONFIG_DIR "${CMAKE_CURRENT_BINARY_DIR}/wslay_enclave")
      file(MAKE_DIRECTORY "${CANOPY_WSLAY_ENCLAVE_CONFIG_DIR}")
      file(
        WRITE "${CANOPY_WSLAY_ENCLAVE_CONFIG_DIR}/config.h"
        "/* Generated by Canopy for enclave-built wslay. */\n"
        "#ifndef CONFIG_H\n"
        "#define CONFIG_H\n"
        "/* No host networking headers are available inside SGX enclaves. */\n"
        "#endif\n")

      add_library(
        wslay_enclave STATIC
        ${CMAKE_CURRENT_SOURCE_DIR}/wslay/lib/wslay_event.c ${CMAKE_CURRENT_SOURCE_DIR}/wslay/lib/wslay_frame.c
        ${CMAKE_CURRENT_SOURCE_DIR}/wslay/lib/wslay_net.c ${CMAKE_CURRENT_SOURCE_DIR}/wslay/lib/wslay_queue.c)
      set_property(TARGET wslay_enclave PROPERTY C_STANDARD 99)
      target_include_directories(
        wslay_enclave
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/wslay/lib/includes ${CMAKE_CURRENT_BINARY_DIR}/wslay/lib/includes
        PRIVATE ${CANOPY_ENCLAVE_LIBCXX_INCLUDES} ${CANOPY_WSLAY_ENCLAVE_CONFIG_DIR})
      target_compile_definitions(wslay_enclave PRIVATE ${CANOPY_ENCLAVE_DEFINES} HAVE_CONFIG_H)
      target_compile_options(
        wslay_enclave
        PRIVATE
          ${CANOPY_ENCLAVE_COMPILE_OPTIONS}
          ${CANOPY_WARN_OK_ENCLAVE}
          $<$<COMPILE_LANG_AND_ID:C,Clang>:-Wno-unused-command-line-argument>
          $<$<COMPILE_LANG_AND_ID:C,Clang>:-Wno-implicit-fallthrough>
          -include
          ${CANOPY_CPP_SOURCE_DIR}/streaming/websocket/include/streaming/websocket/detail/wslay_enclave_net_compat.h)
      target_link_options(wslay_enclave PRIVATE ${CANOPY_ENCLAVE_LINK_OPTIONS})
    endif()
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

if(CANOPY_BUILD_MBEDTLS)
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/mbedtls/CMakeLists.txt")
    message(
      FATAL_ERROR
        "Mbed TLS support is enabled, but c++/submodules/mbedtls is not initialised. Run:\n"
        "  git submodule update --init --recursive c++/submodules/mbedtls\n"
        "or reconfigure with -DCANOPY_BUILD_MBEDTLS=OFF / -DCANOPY_SECURE_STREAM_BACKEND=OPENSSL.")
  endif()

  set(MBEDTLS_TARGET_PREFIX
      "canopy_external_"
      CACHE STRING "Prefix Mbed TLS targets when built by Canopy" FORCE)
  set(ENABLE_PROGRAMS
      OFF
      CACHE BOOL "Do not build Mbed TLS programs inside Canopy" FORCE)
  set(ENABLE_TESTING
      OFF
      CACHE BOOL "Do not build Mbed TLS tests inside Canopy" FORCE)
  set(USE_STATIC_MBEDTLS_LIBRARY
      ON
      CACHE BOOL "Build Mbed TLS static libraries for Canopy" FORCE)
  set(USE_SHARED_MBEDTLS_LIBRARY
      OFF
      CACHE BOOL "Do not build Mbed TLS shared libraries for Canopy" FORCE)
  set(DISABLE_PACKAGE_CONFIG_AND_INSTALL
      ON
      CACHE BOOL "Do not install Mbed TLS package metadata from the Canopy build" FORCE)
  set(MBEDTLS_FATAL_WARNINGS
      OFF
      CACHE BOOL "Do not treat external Mbed TLS warnings as fatal in Canopy" FORCE)
  set(LINK_WITH_PTHREAD
      OFF
      CACHE BOOL "Do not force pthread linkage from Mbed TLS" FORCE)
  set(GEN_FILES
      OFF
      CACHE BOOL "Use generated files from the pinned Mbed TLS release tag" FORCE)

  add_subdirectory(mbedtls)

  foreach(_canopy_mbedtls_target
          canopy_external_mbedtls
          canopy_external_mbedx509
          canopy_external_tfpsacrypto
          canopy_external_mbedcrypto
          mbedtls
          mbedx509
          tfpsacrypto
          mbedcrypto)
    if(TARGET ${_canopy_mbedtls_target})
      set_target_properties(${_canopy_mbedtls_target} PROPERTIES C_CLANG_TIDY "")
    endif()
  endforeach()

  set(_canopy_mbedtls_tls_target)
  foreach(_canopy_mbedtls_candidate canopy_external_mbedtls mbedtls)
    if(TARGET ${_canopy_mbedtls_candidate})
      set(_canopy_mbedtls_tls_target ${_canopy_mbedtls_candidate})
      break()
    endif()
  endforeach()

  set(_canopy_mbedtls_x509_target)
  foreach(_canopy_mbedtls_candidate canopy_external_mbedx509 mbedx509)
    if(TARGET ${_canopy_mbedtls_candidate})
      set(_canopy_mbedtls_x509_target ${_canopy_mbedtls_candidate})
      break()
    endif()
  endforeach()

  set(_canopy_mbedtls_crypto_target)
  foreach(_canopy_mbedtls_candidate canopy_external_tfpsacrypto canopy_external_mbedcrypto tfpsacrypto mbedcrypto)
    if(TARGET ${_canopy_mbedtls_candidate})
      set(_canopy_mbedtls_crypto_target ${_canopy_mbedtls_candidate})
      break()
    endif()
  endforeach()

  if(NOT _canopy_mbedtls_tls_target OR NOT _canopy_mbedtls_x509_target OR NOT _canopy_mbedtls_crypto_target)
    message(FATAL_ERROR "Mbed TLS submodule did not provide the expected TLS, X.509, and crypto CMake targets.")
  endif()

  add_library(canopy_mbedtls_crypto INTERFACE)
  target_link_libraries(canopy_mbedtls_crypto INTERFACE ${_canopy_mbedtls_crypto_target})

  add_library(canopy_mbedtls_x509 INTERFACE)
  target_link_libraries(canopy_mbedtls_x509 INTERFACE ${_canopy_mbedtls_x509_target} canopy_mbedtls_crypto)

  add_library(canopy_mbedtls_tls INTERFACE)
  target_link_libraries(canopy_mbedtls_tls INTERFACE ${_canopy_mbedtls_tls_target} canopy_mbedtls_x509)

  if(CANOPY_BUILD_ENCLAVE AND NOT CANOPY_SGX_BACKEND STREQUAL "Fake")
    function(canopy_add_mbedtls_enclave_archive target source_target)
      get_target_property(_canopy_mbedtls_sources ${source_target} SOURCES)
      get_target_property(_canopy_mbedtls_source_dir ${source_target} SOURCE_DIR)
      if(NOT _canopy_mbedtls_sources OR NOT _canopy_mbedtls_source_dir)
        message(FATAL_ERROR "${source_target} does not expose sources for enclave Mbed TLS build")
      endif()

      set(_canopy_mbedtls_enclave_sources)
      foreach(_canopy_mbedtls_source ${_canopy_mbedtls_sources})
        if(IS_ABSOLUTE "${_canopy_mbedtls_source}")
          list(APPEND _canopy_mbedtls_enclave_sources "${_canopy_mbedtls_source}")
        else()
          list(APPEND _canopy_mbedtls_enclave_sources "${_canopy_mbedtls_source_dir}/${_canopy_mbedtls_source}")
        endif()
      endforeach()

      add_library(${target} STATIC ${_canopy_mbedtls_enclave_sources})
      target_include_directories(
        ${target}
        PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/mbedtls/include>"
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/mbedtls/library"
                "${CMAKE_SOURCE_DIR}/c++/streaming/mbedtls/include"
                ${CANOPY_ENCLAVE_LIBC_INCLUDES})
      target_compile_definitions(
        ${target}
        PUBLIC MBEDTLS_USER_CONFIG_FILE="streaming/mbedtls/sgx_config.h"
        PRIVATE ${CANOPY_ENCLAVE_DEFINES})
      target_compile_options(
        ${target}
        PRIVATE ${CANOPY_ENCLAVE_COMPILE_OPTIONS}
                $<$<COMPILE_LANG_AND_ID:C,Clang>:-Wno-unused-command-line-argument>
                $<$<COMPILE_LANG_AND_ID:C,Clang>:-Wno-missing-declarations>
                $<$<COMPILE_LANG_AND_ID:C,Clang>:-Wno-documentation>
                $<$<COMPILE_LANG_AND_ID:C,Clang>:-Wno-unreachable-code>)
      target_link_options(${target} PRIVATE ${CANOPY_ENCLAVE_LINK_OPTIONS})
    endfunction()

    canopy_add_mbedtls_enclave_archive(canopy_external_mbedcrypto_enclave ${_canopy_mbedtls_crypto_target})
    canopy_add_mbedtls_enclave_archive(canopy_external_mbedx509_enclave ${_canopy_mbedtls_x509_target})
    canopy_add_mbedtls_enclave_archive(canopy_external_mbedtls_enclave ${_canopy_mbedtls_tls_target})
    target_link_libraries(canopy_external_mbedx509_enclave PUBLIC canopy_external_mbedcrypto_enclave)
    target_link_libraries(canopy_external_mbedtls_enclave PUBLIC canopy_external_mbedx509_enclave)

    add_library(canopy_mbedtls_crypto_enclave INTERFACE)
    target_link_libraries(canopy_mbedtls_crypto_enclave INTERFACE canopy_external_mbedcrypto_enclave)

    add_library(canopy_mbedtls_x509_enclave INTERFACE)
    target_link_libraries(canopy_mbedtls_x509_enclave INTERFACE canopy_external_mbedx509_enclave canopy_mbedtls_crypto_enclave)

    add_library(canopy_mbedtls_tls_enclave INTERFACE)
    target_link_libraries(canopy_mbedtls_tls_enclave INTERFACE canopy_external_mbedtls_enclave canopy_mbedtls_x509_enclave)

    message(STATUS "Mbed TLS enclave targets configured")
  endif()

  message(STATUS "Mbed TLS configured from c++/submodules/mbedtls")
endif()
