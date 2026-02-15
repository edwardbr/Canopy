#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.

   Canopy.cmake - Main Build Configuration

   This is the main build configuration file for Canopy. It automatically includes
   platform-specific configuration files:
     - Windows.cmake (on Windows)
     - Linux.cmake (on Linux/Unix)
     - SGX.cmake (when CANOPY_BUILD_ENCLAVE=ON)
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
  option(CANOPY_BUILD "Build host code" ON)
  option(CANOPY_BUILD_EXE "Build executable code" ON)
  option(CANOPY_BUILD_TEST "Build test code" OFF)
  option(CANOPY_BUILD_DEMOS "Build demo code" OFF)
  option(CANOPY_BUILD_COROUTINE "Include coroutine support" OFF)
  option(CANOPY_STANDALONE "Build Canopy stand alone" OFF)
  option(CANOPY_DEBUG_GEN "Get the generator produce verbose messages" OFF)

  # SGX Enclave support (disabled by default - most users don't need this)
  option(CANOPY_BUILD_ENCLAVE "Build SGX enclave code" OFF)

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
  option(CANOPY_USE_THREAD_LOCAL_LOGGING "Turn on thread-local circular buffer logging" OFF)
  option(CANOPY_HANG_ON_FAILED_ASSERT "Hang on failed assert" OFF)
  option(CANOPY_USE_TELEMETRY "Turn on Canopy telemetry" OFF)
  option(CANOPY_USE_CONSOLE_TELEMETRY "Turn on Canopy console telemetry" OFF)
  option(CANOPY_USE_TELEMETRY_RAII_LOGGING "Turn on RAII telemetry logging" OFF)

  # ####################################################################################################################
  # Serialization Encoding Options
  # ####################################################################################################################
  set(CANOPY_DEFAULT_ENCODING
      "PROTOCOL_BUFFERS"
      CACHE STRING "Default encoding format for RPC serialization")
  set_property(
    CACHE CANOPY_DEFAULT_ENCODING
    PROPERTY STRINGS
             "PROTOCOL_BUFFERS"
             "YAS_BINARY"
             "YAS_JSON"
             "YAS_COMPRESSED_BINARY")

  # Validate encoding selection
  if(NOT CANOPY_DEFAULT_ENCODING MATCHES "^(PROTOCOL_BUFFERS|YAS_BINARY|YAS_JSON|YAS_COMPRESSED_BINARY)$")
    message(WARNING "Invalid CANOPY_DEFAULT_ENCODING '${CANOPY_DEFAULT_ENCODING}', defaulting to 'PROTOCOL_BUFFERS'")
    set(CANOPY_DEFAULT_ENCODING
        "PROTOCOL_BUFFERS"
        CACHE STRING "Default encoding format for RPC serialization" FORCE)
  endif()

  # Convert uppercase CMake variable to C++ enum value
  if(CANOPY_DEFAULT_ENCODING STREQUAL "PROTOCOL_BUFFERS")
    set(CANOPY_DEFAULT_ENCODING_VALUE "rpc::encoding::protocol_buffers")
  elseif(CANOPY_DEFAULT_ENCODING STREQUAL "YAS_BINARY")
    set(CANOPY_DEFAULT_ENCODING_VALUE "rpc::encoding::yas_binary")
  elseif(CANOPY_DEFAULT_ENCODING STREQUAL "YAS_JSON")
    set(CANOPY_DEFAULT_ENCODING_VALUE "rpc::encoding::yas_json")
  elseif(CANOPY_DEFAULT_ENCODING STREQUAL "YAS_COMPRESSED_BINARY")
    set(CANOPY_DEFAULT_ENCODING_VALUE "rpc::encoding::yas_compressed_binary")
  else()
    set(CANOPY_DEFAULT_ENCODING_VALUE "rpc::encoding::protocol_buffers")
  endif()

  message("CANOPY_DEFAULT_ENCODING ${CANOPY_DEFAULT_ENCODING} -> ${CANOPY_DEFAULT_ENCODING_VALUE}")

  # ####################################################################################################################
  # Buffer Size Configuration
  # ####################################################################################################################
  if(NOT DEFINED CANOPY_OUT_BUFFER_SIZE)
    # Default to 4KB (default page size for Windows and Linux)
    set(CANOPY_OUT_BUFFER_SIZE 0x1000)
  endif()

  # ####################################################################################################################
  # Build Status Messages
  # ####################################################################################################################
  message("CMAKE_BUILD_TYPE ${CMAKE_BUILD_TYPE}")
  message("CANOPY_BUILD_ENCLAVE ${CANOPY_BUILD_ENCLAVE}")
  message("CANOPY_BUILD ${CANOPY_BUILD}")
  message("CANOPY_BUILD_EXE ${CANOPY_BUILD_EXE}")
  message("CANOPY_BUILD_TEST ${CANOPY_BUILD_TEST}")
  message("CANOPY_BUILD_DEMOS ${CANOPY_BUILD_DEMOS}")
  message("CANOPY_BUILD_COROUTINE ${CANOPY_BUILD_COROUTINE}")
  message("CMAKE_VERBOSE_MAKEFILE ${CMAKE_VERBOSE_MAKEFILE}")
  message("CMAKE_RULE_MESSAGES ${CMAKE_RULE_MESSAGES}")
  message("CANOPY_ENABLE_CLANG_TIDY ${CANOPY_ENABLE_CLANG_TIDY}")
  message("CANOPY_ENABLE_CLANG_TIDY_FIX ${CANOPY_ENABLE_CLANG_TIDY_FIX}")

  # ####################################################################################################################
  # C++ Standard Configuration
  # ####################################################################################################################
  set(CMAKE_CXX_STANDARD 20)
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

  if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    option(GIT_SUBMODULE "Check submodules during build" ON)
    message("doing GIT_SUBMODULE ${GIT_SUBMODULE}")

    if(GIT_SUBMODULE)
      message(STATUS "Submodule init")
      execute_process(
        COMMAND ${GIT_EXECUTABLE} submodule init
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        RESULT_VARIABLE GIT_SUBMOD_INIT_RESULT)

      if(NOT GIT_SUBMOD_INIT_RESULT EQUAL "0")
        message(FATAL_ERROR "submodule init failed")
      endif()

      message(STATUS "Submodule update")
      execute_process(
        COMMAND ${GIT_EXECUTABLE} submodule update
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        RESULT_VARIABLE GIT_SUBMOD_RESULT)

      if(NOT GIT_SUBMOD_RESULT EQUAL "0")
        message(FATAL_ERROR "git submodule update --init failed with ${GIT_SUBMOD_RESULT}, please checkout submodules")
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

  if(CANOPY_USE_THREAD_LOCAL_LOGGING)
    set(CANOPY_USE_THREAD_LOCAL_LOGGING_FLAG CANOPY_USE_THREAD_LOCAL_LOGGING)
  else()
    set(CANOPY_USE_THREAD_LOCAL_LOGGING_FLAG)
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
  else()
    set(CANOPY_BUILD_COROUTINE_FLAG)
  endif()

  set(CANOPY_FMT_LIB fmt::fmt-header-only)

  # ####################################################################################################################
  # Shared Defines (used by both host and enclave builds, all platforms)
  # ####################################################################################################################
  set(CANOPY_SHARED_DEFINES
      _LIB
      NOMINMAX
      _SILENCE_ALL_MS_EXT_DEPRECATION_WARNINGS
      ${CANOPY_USE_LOGGING_FLAG}
      ${CANOPY_USE_THREAD_LOCAL_LOGGING_FLAG}
      ${CANOPY_BUILD_COROUTINE_FLAG}
      ${CANOPY_HANG_ON_FAILED_ASSERT_FLAG}
      ${CANOPY_USE_TELEMETRY_FLAG}
      ${CANOPY_USE_CONSOLE_TELEMETRY_FLAG}
      ${CANOPY_USE_TELEMETRY_RAII_LOGGING_FLAG}
      ${CANOPY_BUILD_TEST_FLAG}
      CANOPY_OUT_BUFFER_SIZE=${CANOPY_OUT_BUFFER_SIZE}
      CANOPY_DEFAULT_ENCODING=${CANOPY_DEFAULT_ENCODING_VALUE})

  # ####################################################################################################################
  # Include Platform-Specific Configuration
  # ####################################################################################################################
  if(WIN32)
    include(${CMAKE_CURRENT_LIST_DIR}/Windows.cmake)
  else()
    include(${CMAKE_CURRENT_LIST_DIR}/Linux.cmake)
  endif()

  # ####################################################################################################################
  # Include SGX Configuration if Enabled
  # ####################################################################################################################
  if(CANOPY_BUILD_ENCLAVE)
    set(CANOPY_ENCLAVE_TARGET "SGX")
    include(${CMAKE_CURRENT_LIST_DIR}/SGX.cmake)
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
  # Remove 'd' suffix to prevent confusion with enclave measurement logic
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
