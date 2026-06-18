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

# wslay + llhttp are both needed by the dual-mode WebSocket framing stream (streaming_websocket) and the dual-mode
# canopy_http_server. In coroutine builds they're also pulled in by the WS transport block below; in blocking builds
# these are the host-side consumers, so we add them eagerly when websocket is enabled regardless of coroutine mode.
if(CANOPY_BUILD_WEBSOCKET AND NOT CANOPY_BUILD_COROUTINE)
  add_subdirectory(wslay)

  set(CANOPY_LLHTTP_GENERATED_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/llhttp/build/c/llhttp.c")
  set(CANOPY_LLHTTP_GENERATED_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/llhttp/build/llhttp.h")
  option(CANOPY_LLHTTP_GENERATE_FROM_TYPESCRIPT "Regenerate llhttp C sources from TypeScript" OFF)

  if(CANOPY_LLHTTP_GENERATE_FROM_TYPESCRIPT
     OR NOT EXISTS "${CANOPY_LLHTTP_GENERATED_SOURCE}"
     OR NOT EXISTS "${CANOPY_LLHTTP_GENERATED_HEADER}")
    find_program(NODE_EXECUTABLE node)
    find_program(NPM_EXECUTABLE npm)
    if(NOT NODE_EXECUTABLE OR NOT NPM_EXECUTABLE)
      message(FATAL_ERROR "llhttp generation requires node and npm")
    endif()
    add_custom_target(
      llhttp_install_deps
      COMMAND ${CMAKE_COMMAND} -E env NPM_CONFIG_LOGLEVEL=error ${NPM_EXECUTABLE} install --ignore-scripts
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/llhttp
      COMMENT "Installing llhttp dependencies")
    add_custom_command(
      OUTPUT ${CANOPY_LLHTTP_GENERATED_SOURCE} ${CANOPY_LLHTTP_GENERATED_HEADER}
      COMMAND ${CMAKE_COMMAND} -E env NODE_PATH=${CMAKE_CURRENT_SOURCE_DIR}/llhttp/node_modules ${NODE_EXECUTABLE}
              --import tsx bin/generate.ts
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/llhttp
      DEPENDS llhttp_install_deps
      COMMENT "Generating llhttp.c and llhttp.h")
    add_custom_target(llhttp_generate DEPENDS ${CANOPY_LLHTTP_GENERATED_SOURCE} ${CANOPY_LLHTTP_GENERATED_HEADER})
  else()
    add_custom_target(llhttp_generate)
  endif()

  add_library(
    llhttp_static STATIC ${CANOPY_LLHTTP_GENERATED_SOURCE} ${CMAKE_CURRENT_SOURCE_DIR}/llhttp/src/native/api.c
                         ${CMAKE_CURRENT_SOURCE_DIR}/llhttp/src/native/http.c)
  add_dependencies(llhttp_static llhttp_generate)
  target_include_directories(llhttp_static PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/llhttp/build
                                                  ${CMAKE_CURRENT_SOURCE_DIR}/llhttp/src/native)
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
  file(WRITE "${CANOPY_CARES_CONFIG_DIR}/c-ares-config.cmake"
       "if(NOT TARGET c-ares::cares)\n" "  message(FATAL_ERROR \"Canopy c-ares target is not available\")\n"
       "endif()\n")
  set(c-ares_DIR
      "${CANOPY_CARES_CONFIG_DIR}"
      CACHE PATH "Canopy c-ares package shim for libcoro" FORCE)

  add_subdirectory(libcoro)

  if(CANOPY_BUILD_WEBSOCKET)
    add_subdirectory(wslay)

    # llhttp for HTTP parsing. Prefer the generated C sources when present so build hosts do not need a modern Node.js
    # toolchain just to compile the untrusted_web transport and demo.
    set(CANOPY_LLHTTP_GENERATED_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/llhttp/build/c/llhttp.c")
    set(CANOPY_LLHTTP_GENERATED_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/llhttp/build/llhttp.h")
    option(CANOPY_LLHTTP_GENERATE_FROM_TYPESCRIPT "Regenerate llhttp C sources from TypeScript" OFF)

    if(CANOPY_LLHTTP_GENERATE_FROM_TYPESCRIPT
       OR NOT EXISTS "${CANOPY_LLHTTP_GENERATED_SOURCE}"
       OR NOT EXISTS "${CANOPY_LLHTTP_GENERATED_HEADER}")
      find_program(NODE_EXECUTABLE node)
      find_program(NPM_EXECUTABLE npm)

      if(NOT NODE_EXECUTABLE OR NOT NPM_EXECUTABLE)
        message(FATAL_ERROR "llhttp generation requires node and npm")
      endif()

      add_custom_target(
        llhttp_install_deps
        COMMAND ${CMAKE_COMMAND} -E env NPM_CONFIG_LOGLEVEL=error ${NPM_EXECUTABLE} install --ignore-scripts
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/llhttp
        COMMENT "Installing llhttp dependencies")

      add_custom_command(
        OUTPUT ${CANOPY_LLHTTP_GENERATED_SOURCE} ${CANOPY_LLHTTP_GENERATED_HEADER}
        COMMAND ${CMAKE_COMMAND} -E env NODE_PATH=${CMAKE_CURRENT_SOURCE_DIR}/llhttp/node_modules ${NODE_EXECUTABLE}
                --import tsx bin/generate.ts
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/llhttp
        DEPENDS llhttp_install_deps
        COMMENT "Generating llhttp.c and llhttp.h")
      add_custom_target(llhttp_generate DEPENDS ${CANOPY_LLHTTP_GENERATED_SOURCE} ${CANOPY_LLHTTP_GENERATED_HEADER})
    else()
      add_custom_target(llhttp_generate)
    endif()

    add_library(
      llhttp_static STATIC ${CANOPY_LLHTTP_GENERATED_SOURCE} ${CMAKE_CURRENT_SOURCE_DIR}/llhttp/src/native/api.c
                           ${CMAKE_CURRENT_SOURCE_DIR}/llhttp/src/native/http.c)
    add_dependencies(llhttp_static llhttp_generate)
    target_include_directories(llhttp_static PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/llhttp/build
                                                    ${CMAKE_CURRENT_SOURCE_DIR}/llhttp/src/native)

  endif()

  if(CANOPY_BUILD_DEMOS
     AND CANOPY_BUILD_WEBSOCKET
     AND NOT CANOPY_WEBSOCKET_DEMO_CALCULATOR_ONLY)
    # llama.cpp has update=none in .gitmodules so it is not cloned by default. cmake/Canopy.cmake selectively
    # initialises it when the full websocket demo is enabled via CANOPY_REQUIRED_SUBMODULES.
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/llama.cpp/CMakeLists.txt")
      message(
        FATAL_ERROR
          "llama.cpp submodule is not initialised. Run:\n" "  git submodule update --init c++/submodules/llama.cpp\n"
          "or reconfigure with -DCANOPY_WEBSOCKET_DEMO_CALCULATOR_ONLY=ON to skip it.")
    endif()
    set(LLAMA_BUILD_COMMON ON)
    # set(GGML_CUDA ON) set(CMAKE_CUDA_ARCHITECTURES 61) set(GGML_CUDA_F16 OFF) set(CMAKE_CUDA_COMPILER
    # /usr/local/cuda/bin/nvcc) set(CMAKE_CUDA_FLAGS "-allow-unsupported-compiler")
    add_subdirectory(llama.cpp)
  endif()

  if(CANOPY_BUILD_DEMOS AND CANOPY_BUILD_WEBSOCKET)
    # libvpx powers the websocket video demo. It is independent of llama and is still needed for
    # CANOPY_WEBSOCKET_DEMO_CALCULATOR_ONLY builds.
    include(CanopyLibvpx)
  endif()
endif()

if(CANOPY_BUILD_WEBSOCKET AND (CANOPY_BUILD_WEBSOCKET_PERMESSAGE_DEFLATE OR CANOPY_BUILD_HTTP_COMPRESSION))
  set(CANOPY_ZLIB_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/zlib")

  if(NOT EXISTS "${CANOPY_ZLIB_SOURCE_DIR}/zlib.h")
    message(
      FATAL_ERROR
        "zlib-backed HTTP/WebSocket compression support is enabled, but the zlib submodule was not "
        "populated at ${CANOPY_ZLIB_SOURCE_DIR}. Keep GIT_SUBMODULE=ON so the Canopy CMake dependency "
        "bootstrap can initialise c++/submodules/zlib, or reconfigure with "
        "-DCANOPY_BUILD_HTTP_COMPRESSION=OFF -DCANOPY_BUILD_WEBSOCKET_PERMESSAGE_DEFLATE=OFF.")
  endif()

  set(CANOPY_ZLIB_CORE_SOURCES
      "${CANOPY_ZLIB_SOURCE_DIR}/adler32.c"
      "${CANOPY_ZLIB_SOURCE_DIR}/crc32.c"
      "${CANOPY_ZLIB_SOURCE_DIR}/deflate.c"
      "${CANOPY_ZLIB_SOURCE_DIR}/inflate.c"
      "${CANOPY_ZLIB_SOURCE_DIR}/inftrees.c"
      "${CANOPY_ZLIB_SOURCE_DIR}/inffast.c"
      "${CANOPY_ZLIB_SOURCE_DIR}/trees.c"
      "${CANOPY_ZLIB_SOURCE_DIR}/zutil.c")

  add_library(canopy_zlib STATIC ${CANOPY_ZLIB_CORE_SOURCES})
  target_include_directories(canopy_zlib PUBLIC "${CANOPY_ZLIB_SOURCE_DIR}")
  target_compile_definitions(canopy_zlib PRIVATE ZLIB_BUILD)
  target_compile_options(canopy_zlib PRIVATE ${CANOPY_COMPILE_OPTIONS})
  target_link_options(canopy_zlib PRIVATE ${CANOPY_LINK_OPTIONS})
  set_target_properties(canopy_zlib PROPERTIES C_CLANG_TIDY "")

  message(STATUS "zlib core configured from ${CANOPY_ZLIB_SOURCE_DIR}")
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

  foreach(
    _canopy_mbedtls_target
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

  if(NOT _canopy_mbedtls_tls_target
     OR NOT _canopy_mbedtls_x509_target
     OR NOT _canopy_mbedtls_crypto_target)
    message(FATAL_ERROR "Mbed TLS submodule did not provide the expected TLS, X.509, and crypto CMake targets.")
  endif()

  add_library(canopy_mbedtls_crypto INTERFACE)
  target_link_libraries(canopy_mbedtls_crypto INTERFACE ${_canopy_mbedtls_crypto_target})

  add_library(canopy_mbedtls_x509 INTERFACE)
  target_link_libraries(canopy_mbedtls_x509 INTERFACE ${_canopy_mbedtls_x509_target} canopy_mbedtls_crypto)

  add_library(canopy_mbedtls_tls INTERFACE)
  target_link_libraries(canopy_mbedtls_tls INTERFACE ${_canopy_mbedtls_tls_target} canopy_mbedtls_x509)

  message(STATUS "Mbed TLS configured from c++/submodules/mbedtls")
endif()

if(CANOPY_BUILD_COMPRESSION)
  set(CANOPY_ZSTD_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/zstd")

  if(NOT EXISTS "${CANOPY_ZSTD_SOURCE_DIR}/build/cmake/CMakeLists.txt")
    message(
      FATAL_ERROR
        "Compression stream support is enabled, but the zstd submodule was not populated at "
        "${CANOPY_ZSTD_SOURCE_DIR}. Keep GIT_SUBMODULE=ON so the Canopy CMake dependency bootstrap "
        "can initialise c++/submodules/zstd, or reconfigure with -DCANOPY_BUILD_COMPRESSION=OFF.")
  endif()

  set(ZSTD_BUILD_PROGRAMS
      OFF
      CACHE BOOL "Disable zstd command line programs" FORCE)
  set(ZSTD_BUILD_TESTS
      OFF
      CACHE BOOL "Disable zstd tests" FORCE)
  set(ZSTD_BUILD_SHARED
      OFF
      CACHE BOOL "Disable zstd shared library" FORCE)
  set(ZSTD_BUILD_STATIC
      ON
      CACHE BOOL "Build zstd static library" FORCE)
  add_subdirectory("${CANOPY_ZSTD_SOURCE_DIR}/build/cmake" "${CMAKE_CURRENT_BINARY_DIR}/zstd")
  message(STATUS "Zstandard configured from ${CANOPY_ZSTD_SOURCE_DIR}")
endif()
