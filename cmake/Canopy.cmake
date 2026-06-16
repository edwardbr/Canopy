#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.

   Canopy.cmake - Main Build Configuration

   This is the main build configuration file for Canopy. It automatically includes
   platform-specific configuration files:
     - Windows.cmake (on Windows)
     - Linux.cmake (on Linux/Unix)
]]

cmake_minimum_required(VERSION 3.24)

message("DEPENDENCIES_LOADED ${DEPENDENCIES_LOADED}")

if(NOT DEPENDENCIES_LOADED)
  message("Configuring Canopy dependencies")

  # Prevent reloading in parent modules
  set(DEPENDENCIES_LOADED ON)

  # ####################################################################################################################
  # Core Build Options
  # ####################################################################################################################
  option(CANOPY_BUILD_TEST "Build test code" OFF)
  option(CANOPY_BUILD_DEMOS "Build demo code" OFF)
  option(CANOPY_BUILD_BENCHMARKING "Build benchmarking code" OFF)
  option(CANOPY_BUILD_RUST "Build the Rust workspace alongside the C++ build" OFF)
  option(CANOPY_BUILD_COROUTINE "Include coroutine support" OFF)
  option(CANOPY_BUILD_PROTOCOL_BUFFERS "Include Protocol Buffers support" ON)
  option(CANOPY_BUILD_NANOPB "Include Nanopb support" ON)
  option(CANOPY_BUILD_CANONICAL_CRYPTO "Include deterministic canonical_crypto serialization support" ON)
  set(CANOPY_BUILD_WEBSOCKET_DEFAULT OFF)
  if(CANOPY_BUILD_COROUTINE AND (CANOPY_BUILD_DEMOS OR CANOPY_BUILD_TEST))
    set(CANOPY_BUILD_WEBSOCKET_DEFAULT ON)
  endif()
  option(CANOPY_BUILD_WEBSOCKET "Include websocket support" ${CANOPY_BUILD_WEBSOCKET_DEFAULT})
  option(CANOPY_VERBOSE_GENERATOR "Get the generator produce verbose messages" OFF)
  option(CANOPY_DEBUG_DEFAULT_DESTRUCTOR "Get the generator produce verbose messages" OFF)
  set(CANOPY_WEBSOCKET_DEMO_CALCULATOR_ONLY_DEFAULT OFF)
  if(NOT CANOPY_BUILD_COROUTINE)
    set(CANOPY_WEBSOCKET_DEMO_CALCULATOR_ONLY_DEFAULT ON)
  endif()
  option(CANOPY_WEBSOCKET_DEMO_CALCULATOR_ONLY
         "Build the websocket demo with only the calculator service, without llama.cpp support"
         ${CANOPY_WEBSOCKET_DEMO_CALCULATOR_ONLY_DEFAULT})
  option(
    CANOPY_IO_URING_SQPOLL
    "Enable io_uring SQPOLL for host io_uring control; turn OFF only for explicit debugging or unsupported kernels."
    ON)
  if(NOT CANOPY_IO_URING_SQPOLL)
    message(WARNING "CANOPY_IO_URING_SQPOLL is OFF; use this only for explicit debugging or unsupported kernels.")
  endif()

  set(CANOPY_ATTESTATION_BACKEND
      "NULL"
      CACHE STRING "Attestation backend selected by security/attestation backend factory")
  set_property(
    CACHE CANOPY_ATTESTATION_BACKEND
    PROPERTY STRINGS
             "FAKE"
             "NULL")
  if(NOT CANOPY_ATTESTATION_BACKEND MATCHES "^(FAKE|NULL)$")
    message(FATAL_ERROR "Invalid CANOPY_ATTESTATION_BACKEND '${CANOPY_ATTESTATION_BACKEND}', expected FAKE or NULL")
  endif()
  set(CANOPY_ATTESTATION_BACKEND_IS_HARDWARE OFF)

  option(
    CANOPY_PRODUCTION_RELEASE
    "Enable production release guardrails. Production releases reject fake attestation."
    OFF)

  if(CANOPY_PRODUCTION_RELEASE)
    if(CANOPY_ATTESTATION_BACKEND STREQUAL "FAKE")
      message(
        FATAL_ERROR
          "CANOPY_PRODUCTION_RELEASE=ON rejects CANOPY_ATTESTATION_BACKEND=${CANOPY_ATTESTATION_BACKEND}. "
          "Use NULL for explicitly unattested production routes.")
    endif()
  endif()

  set(CANOPY_ENABLE_DEVELOPMENT_ATTESTATION_BACKENDS_DEFAULT ON)
  if(CANOPY_PRODUCTION_RELEASE)
    set(CANOPY_ENABLE_DEVELOPMENT_ATTESTATION_BACKENDS_DEFAULT OFF)
  endif()
  option(CANOPY_ENABLE_DEVELOPMENT_ATTESTATION_BACKENDS
         "Build fake attestation backend implementations. This must be OFF for production releases."
         ${CANOPY_ENABLE_DEVELOPMENT_ATTESTATION_BACKENDS_DEFAULT})

  if(CANOPY_PRODUCTION_RELEASE AND CANOPY_ENABLE_DEVELOPMENT_ATTESTATION_BACKENDS)
    message(FATAL_ERROR "CANOPY_PRODUCTION_RELEASE=ON requires CANOPY_ENABLE_DEVELOPMENT_ATTESTATION_BACKENDS=OFF. "
                        "Set CANOPY_PRODUCTION_RELEASE=OFF for release-like fake backend tests.")
  endif()

  if(NOT CANOPY_ENABLE_DEVELOPMENT_ATTESTATION_BACKENDS AND CANOPY_ATTESTATION_BACKEND STREQUAL "FAKE")
    message(
      FATAL_ERROR
        "CANOPY_ENABLE_DEVELOPMENT_ATTESTATION_BACKENDS=OFF rejects CANOPY_ATTESTATION_BACKEND=${CANOPY_ATTESTATION_BACKEND}."
    )
  endif()

  set(CANOPY_SECURITY_ATTESTATION_BUILD_BACKEND_UNIT_TESTS OFF)
  if(CANOPY_BUILD_TEST
     AND CANOPY_ENABLE_DEVELOPMENT_ATTESTATION_BACKENDS
     AND NOT CANOPY_ATTESTATION_BACKEND_IS_HARDWARE)
    set(CANOPY_SECURITY_ATTESTATION_BUILD_BACKEND_UNIT_TESTS ON)
  endif()

  # CANOPY_BUILD_WEBSOCKET works in both coroutine and blocking builds; the WS framing stream (streaming_websocket)
  # operates over a dual-mode streaming::stream and wraps the pure-C wslay library. transport_untrusted_web and higher
  # layers (TLS, http_server, the WS demo) may still gate on CANOPY_BUILD_COROUTINE individually until their own
  # dual-mode work lands.

  set(CANOPY_SECURE_STREAM_BACKEND
      "OPENSSL"
      CACHE STRING "Secure stream backend used by streaming TLS-style adapters")
  set_property(CACHE CANOPY_SECURE_STREAM_BACKEND PROPERTY STRINGS "OPENSSL" "MBEDTLS")
  if(NOT CANOPY_SECURE_STREAM_BACKEND MATCHES "^(OPENSSL|MBEDTLS)$")
    message(
      FATAL_ERROR "Invalid CANOPY_SECURE_STREAM_BACKEND '${CANOPY_SECURE_STREAM_BACKEND}', expected OPENSSL or MBEDTLS")
  endif()

  set(CANOPY_BUILD_MBEDTLS_DEFAULT OFF)
  if(CANOPY_SECURE_STREAM_BACKEND STREQUAL "MBEDTLS")
    set(CANOPY_BUILD_MBEDTLS_DEFAULT ON)
  endif()
  option(CANOPY_BUILD_MBEDTLS "Build bundled Mbed TLS support from c++/submodules/mbedtls"
         ${CANOPY_BUILD_MBEDTLS_DEFAULT})
  if(CANOPY_SECURE_STREAM_BACKEND STREQUAL "MBEDTLS" AND NOT CANOPY_BUILD_MBEDTLS)
    set(CANOPY_BUILD_MBEDTLS
        ON
        CACHE BOOL "Build bundled Mbed TLS support from c++/submodules/mbedtls" FORCE)
  endif()
  option(CANOPY_BUILD_COMPRESSION "Build compression stream support from c++/submodules/zstd" OFF)
  option(CANOPY_BUILD_HTTP_COMPRESSION "Build HTTP gzip content-encoding support from c++/submodules/zlib" ON)
  option(CANOPY_BUILD_WEBSOCKET_PERMESSAGE_DEFLATE
         "Build RFC 7692 permessage-deflate support for WebSocket streams from c++/submodules/zlib" ON)

  # ####################################################################################################################
  # Debug Options
  # ####################################################################################################################
  option(CANOPY_DEBUG_LEAK "Enable leak sanitizer" OFF)
  option(CANOPY_DEBUG_ADDRESS "Enable address sanitizer" OFF)
  option(CANOPY_DEBUG_THREAD "Enable thread sanitizer" OFF)
  option(CANOPY_DEBUG_UNDEFINED "Enable undefined behavior sanitizer" OFF)
  option(CANOPY_DEBUG_ALL "Enable all sanitizers" OFF)

  # ####################################################################################################################
  # Development Options
  # ####################################################################################################################
  option(CANOPY_ENABLE_CLANG_TIDY "Enable clang-tidy in build" OFF)
  option(CANOPY_ENABLE_CLANG_TIDY_FIX "Enable auto-fix in clang-tidy" OFF)
  option(CANOPY_ENABLE_COVERAGE "Enable code coverage" OFF)
  option(CANOPY_FORCE_DEBUG_INFORMATION "Force inclusion of debug information" ON)

  option(CMAKE_VERBOSE_MAKEFILE "Verbose build step" OFF)
  option(CMAKE_RULE_MESSAGES "Verbose cmake" OFF)

  # ####################################################################################################################
  # Logging and Telemetry Options
  # ####################################################################################################################
  option(CANOPY_USE_LOGGING "Turn on Canopy logging" OFF)
  option(CANOPY_ADD_REF_COUNT_CHECKS "Turn on Canopy refcount checks" OFF)
  option(CANOPY_ASSERT_ON_LOGGER_ERROR "Turn on asserts when there is a logger error")
  option(CANOPY_HANG_ON_FAILED_ASSERT "Hang on failed assert" OFF)
  option(CANOPY_USE_TELEMETRY "Turn on Canopy telemetry" OFF)
  option(CANOPY_USE_CONSOLE_TELEMETRY "Turn on Canopy console telemetry" OFF)
  option(CANOPY_USE_TELEMETRY_RAII_LOGGING "Turn on RAII telemetry logging" OFF)

  set(CANOPY_LOGGING_LEVEL
      "2"
      CACHE STRING "Minimum logging level (0=TRACE, 1=DEBUG, 2=INFO, 3=WARNING, 4=ERROR, 5=CRITICAL)")
  set_property(
    CACHE CANOPY_LOGGING_LEVEL
    PROPERTY STRINGS
             "0"
             "1"
             "2"
             "3"
             "4"
             "5")

  if(NOT CANOPY_LOGGING_LEVEL MATCHES "^[0-5]$")
    message(WARNING "Invalid CANOPY_LOGGING_LEVEL '${CANOPY_LOGGING_LEVEL}', defaulting to 2 (INFO)")
    set(CANOPY_LOGGING_LEVEL
        "2"
        CACHE STRING "Minimum logging level (0=TRACE, 1=DEBUG, 2=INFO, 3=WARNING, 4=ERROR, 5=CRITICAL)" FORCE)
  endif()

  # ####################################################################################################################
  # Serialization Encoding Options
  # ####################################################################################################################
  set(CANOPY_DEFAULT_ENCODING
      "NANOPB"
      CACHE STRING "Default encoding format for RPC serialization")
  set_property(
    CACHE CANOPY_DEFAULT_ENCODING
    PROPERTY STRINGS
             "NANOPB"
             "PROTOCOL_BUFFERS"
             "CANONICAL_CRYPTO"
             "YAS_BINARY"
             "YAS_JSON"
             "YAS_COMPRESSED_BINARY")

  # Validate encoding selection
  if(NOT CANOPY_DEFAULT_ENCODING MATCHES
     "^(NANOPB|PROTOCOL_BUFFERS|CANONICAL_CRYPTO|YAS_BINARY|YAS_JSON|YAS_COMPRESSED_BINARY)$")
    message(WARNING "Invalid CANOPY_DEFAULT_ENCODING '${CANOPY_DEFAULT_ENCODING}', defaulting to 'NANOPB'")
    set(CANOPY_DEFAULT_ENCODING
        "NANOPB"
        CACHE STRING "Default encoding format for RPC serialization" FORCE)
  endif()

  # Convert uppercase CMake variable to C++ enum value
  if(CANOPY_DEFAULT_ENCODING STREQUAL "NANOPB")
    if(CANOPY_BUILD_NANOPB OR CANOPY_BUILD_PROTOCOL_BUFFERS)
      if(NOT CANOPY_BUILD_NANOPB)
        message(
          STATUS
            "CANOPY_DEFAULT_ENCODING=NANOPB but CANOPY_BUILD_NANOPB=OFF; rpc::encoding::nanopb will use the Protocol Buffers backend."
        )
      endif()
      set(CANOPY_DEFAULT_ENCODING_VALUE "rpc::encoding::nanopb")
    else()
      message(WARNING "CANOPY_DEFAULT_ENCODING=NANOPB but CANOPY_BUILD_NANOPB=OFF; defaulting to YAS_BINARY")
      set(CANOPY_DEFAULT_ENCODING "YAS_BINARY")
      set(CANOPY_DEFAULT_ENCODING_VALUE "rpc::encoding::yas_binary")
    endif()
  elseif(CANOPY_DEFAULT_ENCODING STREQUAL "PROTOCOL_BUFFERS")
    if(CANOPY_BUILD_PROTOCOL_BUFFERS OR CANOPY_BUILD_NANOPB)
      if(NOT CANOPY_BUILD_PROTOCOL_BUFFERS)
        message(
          STATUS
            "CANOPY_DEFAULT_ENCODING=PROTOCOL_BUFFERS but CANOPY_BUILD_PROTOCOL_BUFFERS=OFF; rpc::encoding::protocol_buffers will use the Nanopb backend."
        )
      endif()
      set(CANOPY_DEFAULT_ENCODING_VALUE "rpc::encoding::protocol_buffers")
    else()
      message(
        WARNING
          "CANOPY_DEFAULT_ENCODING=PROTOCOL_BUFFERS but CANOPY_BUILD_PROTOCOL_BUFFERS=OFF; defaulting to YAS_BINARY")
      set(CANOPY_DEFAULT_ENCODING "YAS_BINARY")
      set(CANOPY_DEFAULT_ENCODING_VALUE "rpc::encoding::yas_binary")
    endif()
  elseif(CANOPY_DEFAULT_ENCODING STREQUAL "CANONICAL_CRYPTO")
    if(CANOPY_BUILD_CANONICAL_CRYPTO)
      set(CANOPY_DEFAULT_ENCODING_VALUE "rpc::encoding::canonical_crypto")
    else()
      message(
        WARNING
          "CANOPY_DEFAULT_ENCODING=CANONICAL_CRYPTO but CANOPY_BUILD_CANONICAL_CRYPTO=OFF; defaulting to YAS_BINARY")
      set(CANOPY_DEFAULT_ENCODING "YAS_BINARY")
      set(CANOPY_DEFAULT_ENCODING_VALUE "rpc::encoding::yas_binary")
    endif()
  elseif(CANOPY_DEFAULT_ENCODING STREQUAL "YAS_BINARY")
    set(CANOPY_DEFAULT_ENCODING_VALUE "rpc::encoding::yas_binary")
  elseif(CANOPY_DEFAULT_ENCODING STREQUAL "YAS_JSON")
    set(CANOPY_DEFAULT_ENCODING_VALUE "rpc::encoding::yas_json")
  elseif(CANOPY_DEFAULT_ENCODING STREQUAL "YAS_COMPRESSED_BINARY")
    set(CANOPY_DEFAULT_ENCODING_VALUE "rpc::encoding::yas_compressed_binary")
  else()
    set(CANOPY_DEFAULT_ENCODING_VALUE "rpc::encoding::nanopb")
  endif()

  message("CANOPY_DEFAULT_ENCODING ${CANOPY_DEFAULT_ENCODING} -> ${CANOPY_DEFAULT_ENCODING_VALUE}")

  # ####################################################################################################################
  # Buffer Size Configuration
  # ####################################################################################################################
  if(NOT DEFINED CANOPY_OUT_BUFFER_SIZE)
    # Default to 4KB(default page size for Windows and Linux)
    set(CANOPY_OUT_BUFFER_SIZE 0x1000)
  endif()

  # ####################################################################################################################
  # Build Status Messages
  # ####################################################################################################################
  message("CMAKE_BUILD_TYPE ${CMAKE_BUILD_TYPE}")
  message("CANOPY_BUILD_EXE ${CANOPY_BUILD_EXE}")
  message("CANOPY_BUILD_TEST ${CANOPY_BUILD_TEST}")
  message("CANOPY_BUILD_DEMOS ${CANOPY_BUILD_DEMOS}")
  message("CANOPY_BUILD_PROTOCOL_BUFFERS ${CANOPY_BUILD_PROTOCOL_BUFFERS}")
  message("CANOPY_BUILD_NANOPB ${CANOPY_BUILD_NANOPB}")
  message("CANOPY_BUILD_CANONICAL_CRYPTO ${CANOPY_BUILD_CANONICAL_CRYPTO}")
  message("CANOPY_BUILD_WEBSOCKET ${CANOPY_BUILD_WEBSOCKET}")
  message("CANOPY_WEBSOCKET_DEMO_CALCULATOR_ONLY ${CANOPY_WEBSOCKET_DEMO_CALCULATOR_ONLY}")
  message("CANOPY_BUILD_COROUTINE ${CANOPY_BUILD_COROUTINE}")
  message("CANOPY_SECURE_STREAM_BACKEND ${CANOPY_SECURE_STREAM_BACKEND}")
  message("CANOPY_ATTESTATION_BACKEND ${CANOPY_ATTESTATION_BACKEND}")
  message("CANOPY_PRODUCTION_RELEASE ${CANOPY_PRODUCTION_RELEASE}")
  message("CANOPY_ENABLE_DEVELOPMENT_ATTESTATION_BACKENDS ${CANOPY_ENABLE_DEVELOPMENT_ATTESTATION_BACKENDS}")
  message("CANOPY_BUILD_MBEDTLS ${CANOPY_BUILD_MBEDTLS}")
  message("CANOPY_BUILD_COMPRESSION ${CANOPY_BUILD_COMPRESSION}")
  message("CMAKE_VERBOSE_MAKEFILE ${CMAKE_VERBOSE_MAKEFILE}")
  message("CMAKE_RULE_MESSAGES ${CMAKE_RULE_MESSAGES}")
  message("CANOPY_ENABLE_CLANG_TIDY ${CANOPY_ENABLE_CLANG_TIDY}")
  message("CANOPY_ENABLE_CLANG_TIDY_FIX ${CANOPY_ENABLE_CLANG_TIDY_FIX}")
  message("CANOPY_LOGGING_LEVEL ${CANOPY_LOGGING_LEVEL}")

  # ####################################################################################################################
  # C++ Standard Configuration
  # ####################################################################################################################
  if(CANOPY_BUILD_COROUTINE)
    set(CMAKE_CXX_STANDARD 20)
  else()
    set(CMAKE_CXX_STANDARD 17)
  endif()
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)

  list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}")

  # ####################################################################################################################
  # Output Directories
  # ####################################################################################################################

  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
  set(CMAKE_PDB_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
  set(CMAKE_COMPILE_PDB_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)

  # ####################################################################################################################
  # Install Location
  # ####################################################################################################################
  message(INSTALL_LOCATION ${INSTALL_LOCATION})

  if(DEFINED INSTALL_LOCATION)
    message(CMAKE_INSTALL_PREFIX ${INSTALL_LOCATION})
    set(CMAKE_INSTALL_PREFIX
        ${INSTALL_LOCATION}
        CACHE STRING "Override location of installation files" FORCE)
  else()
    set(CMAKE_INSTALL_PREFIX
        ${CMAKE_BINARY_DIR}/install
        CACHE STRING "Override location of installation files" FORCE)
  endif()

  # ####################################################################################################################
  # Git Submodules
  # ####################################################################################################################
  find_package(Git QUIET)
  message("submodules GIT_FOUND ${GIT_FOUND}")

  if(GIT_FOUND)
    if(EXISTS "${CMAKE_SOURCE_DIR}/.git")
      message("found .git directory")
    elseif(EXISTS "${CMAKE_SOURCE_DIR}/.git")
      message("found .git file")
    else()
      message("NO .git file or directory")
    endif()

    option(GIT_SUBMODULE "Check submodules during build" ON)
    message("doing GIT_SUBMODULE ${GIT_SUBMODULE}")

    if(GIT_SUBMODULE)
      message(STATUS "CANOPY_BUILD_PROTOCOL_BUFFERS: ${CANOPY_BUILD_PROTOCOL_BUFFERS}")
      message(STATUS "CANOPY_BUILD_COROUTINE: ${CANOPY_BUILD_COROUTINE}")
      message(STATUS "CANOPY_BUILD_TEST: ${CANOPY_BUILD_TEST}")
      message(STATUS "CANOPY_BUILD_WEBSOCKET: ${CANOPY_BUILD_WEBSOCKET}")
      message(STATUS "CANOPY_BUILD_COMPRESSION: ${CANOPY_BUILD_COMPRESSION}")
      message(STATUS "CANOPY_BUILD_DEMOS: ${CANOPY_BUILD_DEMOS}")
      set(CANOPY_REQUIRED_SUBMODULES
          c++/submodules/yas
          c++/submodules/fmt
          submodules/idlparser
          c++/submodules/spdlog
          c++/submodules/args)

      if(CANOPY_BUILD_COROUTINE)
        list(APPEND CANOPY_REQUIRED_SUBMODULES c++/submodules/libcoro c++/submodules/c-ares)
      endif()
      if(CANOPY_BUILD_PROTOCOL_BUFFERS OR CANOPY_BUILD_NANOPB)
        list(APPEND CANOPY_REQUIRED_SUBMODULES submodules/protobuf c++/submodules/nanopb)
      endif()
      if(CANOPY_BUILD_TEST OR CANOPY_BUILD_GTEST)
        list(APPEND CANOPY_REQUIRED_SUBMODULES c++/submodules/googletest)
      endif()
      if(CANOPY_BUILD_WEBSOCKET)
        list(APPEND CANOPY_REQUIRED_SUBMODULES c++/submodules/wslay c++/submodules/llhttp)
      endif()
      if(CANOPY_BUILD_WEBSOCKET AND (CANOPY_BUILD_WEBSOCKET_PERMESSAGE_DEFLATE OR CANOPY_BUILD_HTTP_COMPRESSION))
        list(APPEND CANOPY_REQUIRED_SUBMODULES c++/submodules/zlib)
      endif()
      if(CANOPY_BUILD_MBEDTLS)
        list(APPEND CANOPY_REQUIRED_SUBMODULES c++/submodules/mbedtls)
      endif()
      if(CANOPY_BUILD_COMPRESSION)
        list(APPEND CANOPY_REQUIRED_SUBMODULES c++/submodules/zstd)
      endif()
      if(CANOPY_BUILD_DEMOS
         AND CANOPY_BUILD_WEBSOCKET
         AND NOT CANOPY_WEBSOCKET_DEMO_CALCULATOR_ONLY)
        list(APPEND CANOPY_REQUIRED_SUBMODULES c++/submodules/llama.cpp)
      endif()
      if(CANOPY_BUILD_DEMOS AND CANOPY_BUILD_WEBSOCKET)
        # libvpx is needed by the websocket video demo even when llama is disabled by
        # CANOPY_WEBSOCKET_DEMO_CALCULATOR_ONLY.
        list(APPEND CANOPY_REQUIRED_SUBMODULES c++/submodules/libvpx)
      endif()

      set(CANOPY_FULL_HISTORY_SUBMODULES c++/submodules/yas c++/submodules/fmt submodules/idlparser c++/submodules/args)
      set(CANOPY_BRANCH_PINNED_SUBMODULES)
      set(CANOPY_RECURSIVE_SUBMODULES c++/submodules/mbedtls)

      function(canopy_submodule_is_populated submodule_path out_var)
        set(submodule_root "${CMAKE_CURRENT_LIST_DIR}/../${submodule_path}")

        if(NOT IS_DIRECTORY "${submodule_root}")
          set(${out_var}
              FALSE
              PARENT_SCOPE)
          return()
        endif()

        file(
          GLOB submodule_entries
          LIST_DIRECTORIES true
          RELATIVE "${submodule_root}"
          "${submodule_root}/*")

        list(REMOVE_ITEM submodule_entries ".git")

        list(LENGTH submodule_entries submodule_entry_count)
        if(submodule_entry_count EQUAL 0)
          set(${out_var}
              FALSE
              PARENT_SCOPE)
          return()
        endif()

        set(${out_var}
            TRUE
            PARENT_SCOPE)
      endfunction()

      set(CANOPY_MISSING_SUBMODULES)
      foreach(CANOPY_SUBMODULE_PATH IN LISTS CANOPY_REQUIRED_SUBMODULES)
        canopy_submodule_is_populated("${CANOPY_SUBMODULE_PATH}" CANOPY_SUBMODULE_PRESENT)
        if(NOT CANOPY_SUBMODULE_PRESENT)
          list(APPEND CANOPY_MISSING_SUBMODULES ${CANOPY_SUBMODULE_PATH})
        endif()
      endforeach()

      if(NOT CANOPY_MISSING_SUBMODULES)
        message(STATUS "Required submodules already present on disk; skipping git submodule init")
      else()
        foreach(CANOPY_SUBMODULE_PATH IN LISTS CANOPY_MISSING_SUBMODULES)
          if(CANOPY_SUBMODULE_PATH IN_LIST CANOPY_BRANCH_PINNED_SUBMODULES)
            message(STATUS "Submodule update (tracked branch): ${CANOPY_SUBMODULE_PATH}")
            execute_process(
              COMMAND ${GIT_EXECUTABLE} submodule update --init --checkout --depth 1 -- ${CANOPY_SUBMODULE_PATH}
              WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/.."
              RESULT_VARIABLE GIT_SUBMOD_RESULT)
            if(GIT_SUBMOD_RESULT EQUAL "0")
              set(CANOPY_SUBMODULE_BRANCH "")
              execute_process(
                COMMAND ${GIT_EXECUTABLE} config -f .gitmodules --get submodule.${CANOPY_SUBMODULE_PATH}.branch
                WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/.."
                OUTPUT_VARIABLE CANOPY_SUBMODULE_BRANCH
                OUTPUT_STRIP_TRAILING_WHITESPACE
                RESULT_VARIABLE GIT_SUBMOD_RESULT)
            endif()
            if(GIT_SUBMOD_RESULT EQUAL "0" AND NOT CANOPY_SUBMODULE_BRANCH)
              message(FATAL_ERROR "Branch-pinned submodule ${CANOPY_SUBMODULE_PATH} has no branch in .gitmodules")
            endif()
            if(GIT_SUBMOD_RESULT EQUAL "0" AND CANOPY_SUBMODULE_BRANCH)
              execute_process(
                COMMAND ${GIT_EXECUTABLE} -C ${CANOPY_SUBMODULE_PATH} fetch --depth 1 origin
                        ${CANOPY_SUBMODULE_BRANCH}:refs/remotes/origin/${CANOPY_SUBMODULE_BRANCH}
                WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/.."
                RESULT_VARIABLE GIT_SUBMOD_RESULT)
            endif()
            if(GIT_SUBMOD_RESULT EQUAL "0" AND CANOPY_SUBMODULE_BRANCH)
              execute_process(
                COMMAND ${GIT_EXECUTABLE} -C ${CANOPY_SUBMODULE_PATH} checkout --force -B ${CANOPY_SUBMODULE_BRANCH}
                        refs/remotes/origin/${CANOPY_SUBMODULE_BRANCH}
                WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/.."
                RESULT_VARIABLE GIT_SUBMOD_RESULT)
            endif()
          elseif(CANOPY_SUBMODULE_PATH IN_LIST CANOPY_FULL_HISTORY_SUBMODULES)
            message(STATUS "Submodule update: ${CANOPY_SUBMODULE_PATH}")
            execute_process(
              COMMAND ${GIT_EXECUTABLE} submodule update --init --checkout -- ${CANOPY_SUBMODULE_PATH}
              WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/.."
              RESULT_VARIABLE GIT_SUBMOD_RESULT)
          elseif(CANOPY_SUBMODULE_PATH IN_LIST CANOPY_RECURSIVE_SUBMODULES)
            message(STATUS "Submodule update (recursive shallow): ${CANOPY_SUBMODULE_PATH}")
            execute_process(
              COMMAND ${GIT_EXECUTABLE} submodule update --init --checkout --recursive --depth 1 --
                      ${CANOPY_SUBMODULE_PATH}
              WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/.."
              RESULT_VARIABLE GIT_SUBMOD_RESULT)
          else()
            message(STATUS "Submodule update (shallow): ${CANOPY_SUBMODULE_PATH}")
            execute_process(
              COMMAND ${GIT_EXECUTABLE} submodule update --init --checkout --depth 1 -- ${CANOPY_SUBMODULE_PATH}
              WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/.."
              RESULT_VARIABLE GIT_SUBMOD_RESULT)
          endif()

          if(NOT GIT_SUBMOD_RESULT EQUAL "0")
            message(FATAL_ERROR "git submodule update failed for ${CANOPY_SUBMODULE_PATH} with ${GIT_SUBMOD_RESULT}. "
                                "The repository metadata for that submodule path is likely inconsistent.")
          endif()

          canopy_submodule_is_populated("${CANOPY_SUBMODULE_PATH}" CANOPY_SUBMODULE_PRESENT)
          if(NOT CANOPY_SUBMODULE_PRESENT)
            message(FATAL_ERROR "submodule update completed but ${CANOPY_SUBMODULE_PATH} is still not populated")
          endif()
        endforeach()
      endif()
    endif()
  endif()

  # ####################################################################################################################
  # Convert Options to Compile Flags
  # ####################################################################################################################
  if(CANOPY_BUILD_TEST)
    set(CANOPY_BUILD_TEST_FLAG CANOPY_BUILD_TEST)
  else()
    set(CANOPY_BUILD_TEST_FLAG)
  endif()

  if(CANOPY_USE_LOGGING)
    set(CANOPY_USE_LOGGING_FLAG CANOPY_USE_LOGGING)
  else()
    set(CANOPY_USE_LOGGING_FLAG)
  endif()

  if(CANOPY_ADD_REF_COUNT_CHECKS)
    set(CANOPY_ADD_REF_COUNT_CHECKS_FLAG CANOPY_ADD_REF_COUNT_CHECKS)
  else()
    set(CANOPY_ADD_REF_COUNT_CHECKS_FLAG)
  endif()

  if(CANOPY_ASSERT_ON_LOGGER_ERROR)
    set(CANOPY_ASSERT_ON_LOGGER_ERROR_FLAG CANOPY_ASSERT_ON_LOGGER_ERROR)
  else()
    set(CANOPY_ASSERT_ON_LOGGER_ERROR_FLAG)
  endif()

  if(CANOPY_HANG_ON_FAILED_ASSERT)
    set(CANOPY_HANG_ON_FAILED_ASSERT_FLAG CANOPY_HANG_ON_FAILED_ASSERT)
  else()
    set(CANOPY_HANG_ON_FAILED_ASSERT_FLAG)
  endif()

  if(CANOPY_USE_TELEMETRY)
    set(CANOPY_USE_TELEMETRY_FLAG CANOPY_USE_TELEMETRY)
  else()
    set(CANOPY_USE_TELEMETRY_FLAG)
  endif()

  if(CANOPY_USE_CONSOLE_TELEMETRY)
    set(CANOPY_USE_CONSOLE_TELEMETRY_FLAG CANOPY_USE_CONSOLE_TELEMETRY)
  else()
    set(CANOPY_USE_CONSOLE_TELEMETRY_FLAG)
  endif()

  if(CANOPY_USE_TELEMETRY_RAII_LOGGING)
    set(CANOPY_USE_TELEMETRY_RAII_LOGGING_FLAG CANOPY_USE_TELEMETRY_RAII_LOGGING)
  else()
    set(CANOPY_USE_TELEMETRY_RAII_LOGGING_FLAG)
  endif()

  if(${CANOPY_BUILD_COROUTINE})
    set(CANOPY_BUILD_COROUTINE_FLAG CANOPY_BUILD_COROUTINE)
    set(CANOPY_CORO_RUNTIME libcoro)
  else()
    set(CANOPY_BUILD_COROUTINE_FLAG)
  endif()

  if(${CANOPY_BUILD_PROTOCOL_BUFFERS})
    set(CANOPY_BUILD_PROTOCOL_BUFFERS_FLAG CANOPY_BUILD_PROTOCOL_BUFFERS)
  else()
    set(CANOPY_BUILD_PROTOCOL_BUFFERS_FLAG)
  endif()

  if(${CANOPY_BUILD_NANOPB})
    set(CANOPY_BUILD_NANOPB_FLAG CANOPY_BUILD_NANOPB)
  else()
    set(CANOPY_BUILD_NANOPB_FLAG)
  endif()

  if(${CANOPY_BUILD_CANONICAL_CRYPTO})
    set(CANOPY_BUILD_CANONICAL_CRYPTO_FLAG CANOPY_BUILD_CANONICAL_CRYPTO)
  else()
    set(CANOPY_BUILD_CANONICAL_CRYPTO_FLAG)
  endif()

  if(CANOPY_BUILD_NANOPB AND NOT CANOPY_BUILD_PROTOCOL_BUFFERS)
    set(CANOPY_PROTOBUF_ENCODING_ALIAS_FLAG CANOPY_USE_NANOPB_FOR_PROTOCOL_BUFFERS)
  elseif(CANOPY_BUILD_PROTOCOL_BUFFERS AND NOT CANOPY_BUILD_NANOPB)
    set(CANOPY_PROTOBUF_ENCODING_ALIAS_FLAG CANOPY_USE_PROTOCOL_BUFFERS_FOR_NANOPB)
  else()
    set(CANOPY_PROTOBUF_ENCODING_ALIAS_FLAG)
  endif()

  if(${CANOPY_DEBUG_DEFAULT_DESTRUCTOR})
    set(CANOPY_DEBUG_DEFAULT_DESTRUCTOR_FLAG CANOPY_DEBUG_DEFAULT_DESTRUCTOR)
  else()
    set(CANOPY_DEBUG_DEFAULT_DESTRUCTOR_FLAG)
  endif()

  if(CANOPY_IO_URING_SQPOLL)
    set(CANOPY_IO_URING_SQPOLL_FLAG CANOPY_IO_URING_SQPOLL)
  else()
    set(CANOPY_IO_URING_SQPOLL_FLAG)
  endif()

  # OpenSSL TLS is dual-mode (streaming_openssl_tls drives memory BIOs over the dual-mode streaming::stream interface). mbedtls
  # is still coroutine-only.
  if(CANOPY_SECURE_STREAM_BACKEND STREQUAL "MBEDTLS")
    if(CANOPY_BUILD_COROUTINE)
      set(CANOPY_SECURE_STREAM_BACKEND_FLAG CANOPY_SECURE_STREAM_BACKEND_MBEDTLS)
    else()
      set(CANOPY_SECURE_STREAM_BACKEND_FLAG)
    endif()
  else()
    set(CANOPY_SECURE_STREAM_BACKEND_FLAG CANOPY_SECURE_STREAM_BACKEND_OPENSSL)
  endif()
  if(CANOPY_ATTESTATION_BACKEND STREQUAL "NULL")
    set(CANOPY_ATTESTATION_BACKEND_FLAG CANOPY_ATTESTATION_BACKEND_NULL)
  elseif(CANOPY_ATTESTATION_BACKEND STREQUAL "FAKE")
    set(CANOPY_ATTESTATION_BACKEND_FLAG CANOPY_ATTESTATION_BACKEND_FAKE)
  else()
    set(CANOPY_ATTESTATION_BACKEND_FLAG)
  endif()

  set(CANOPY_LOGGING_LEVEL_FLAG CANOPY_LOGGING_LEVEL=${CANOPY_LOGGING_LEVEL})

  if(CANOPY_PRODUCTION_RELEASE)
    set(CANOPY_PRODUCTION_RELEASE_FLAG CANOPY_PRODUCTION_RELEASE)
  else()
    set(CANOPY_PRODUCTION_RELEASE_FLAG)
  endif()

  if(CANOPY_ENABLE_DEVELOPMENT_ATTESTATION_BACKENDS)
    set(CANOPY_ENABLE_DEVELOPMENT_ATTESTATION_BACKENDS_FLAG CANOPY_ENABLE_DEVELOPMENT_ATTESTATION_BACKENDS)
  else()
    set(CANOPY_ENABLE_DEVELOPMENT_ATTESTATION_BACKENDS_FLAG)
  endif()

  set(CANOPY_FMT_LIB fmt::fmt-header-only)

  # ####################################################################################################################
  # Shared Defines(used by host builds, all platforms)
  # ####################################################################################################################
  set(CANOPY_SHARED_DEFINES
      _LIB
      NOMINMAX
      _SILENCE_ALL_MS_EXT_DEPRECATION_WARNINGS
      ${CANOPY_USE_LOGGING_FLAG}
      ${CANOPY_ADD_REF_COUNT_CHECKS_FLAG}
      ${CANOPY_ASSERT_ON_LOGGER_ERROR_FLAG}
      ${CANOPY_BUILD_COROUTINE_FLAG}
      ${CANOPY_BUILD_PROTOCOL_BUFFERS_FLAG}
      ${CANOPY_BUILD_NANOPB_FLAG}
      ${CANOPY_BUILD_CANONICAL_CRYPTO_FLAG}
      ${CANOPY_PROTOBUF_ENCODING_ALIAS_FLAG}
      ${CANOPY_HANG_ON_FAILED_ASSERT_FLAG}
      ${CANOPY_USE_TELEMETRY_FLAG}
      ${CANOPY_USE_CONSOLE_TELEMETRY_FLAG}
      ${CANOPY_USE_TELEMETRY_RAII_LOGGING_FLAG}
      ${CANOPY_BUILD_TEST_FLAG}
      ${CANOPY_DEBUG_DEFAULT_DESTRUCTOR_FLAG}
      ${CANOPY_IO_URING_SQPOLL_FLAG}
      ${CANOPY_SECURE_STREAM_BACKEND_FLAG}
      ${CANOPY_ATTESTATION_BACKEND_FLAG}
      ${CANOPY_PRODUCTION_RELEASE_FLAG}
      ${CANOPY_ENABLE_DEVELOPMENT_ATTESTATION_BACKENDS_FLAG}
      CANOPY_OUT_BUFFER_SIZE=${CANOPY_OUT_BUFFER_SIZE}
      CANOPY_DEFAULT_ENCODING=${CANOPY_DEFAULT_ENCODING_VALUE}
      ${CANOPY_LOGGING_LEVEL_FLAG})

  # ####################################################################################################################
  # Include Platform - Specific Configuration
  # ####################################################################################################################
  if(WIN32)
    include(${CMAKE_CURRENT_LIST_DIR}/Windows.cmake)
  else()
    include(${CMAKE_CURRENT_LIST_DIR}/Linux.cmake)
  endif()

  # ####################################################################################################################
  # Output Configuration Summary
  # ####################################################################################################################
  message("CANOPY_DEFINES ${CANOPY_DEFINES}")
  message("CANOPY_COMPILE_OPTIONS ${CANOPY_COMPILE_OPTIONS}")
  message("CANOPY_LINK_OPTIONS ${CANOPY_LINK_OPTIONS}")
  message("CANOPY_LINK_EXE_OPTIONS ${CANOPY_LINK_EXE_OPTIONS}")
  message("CANOPY_DEBUG_OPTIONS ${CANOPY_DEBUG_OPTIONS}")

  # ####################################################################################################################
  # Debug Postfix Configuration
  # ####################################################################################################################
  # Remove 'd' suffix to keep artifact names stable across build types
  set(CMAKE_DEBUG_POSTFIX
      ""
      CACHE STRING "Adds a postfix for debug-built libraries." FORCE)

  # ####################################################################################################################
  # Testing Configuration
  # ####################################################################################################################
  if(CANOPY_BUILD_TEST)
    include(GoogleTest)
    enable_testing()
  endif()

endif(NOT DEPENDENCIES_LOADED)
