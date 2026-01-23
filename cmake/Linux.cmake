#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.

   Linux Platform Configuration for Canopy

   This file is automatically included on Linux platforms.
   It provides Linux-specific compilation flags, defines, and settings.
]]

cmake_minimum_required(VERSION 3.24)

if(WIN32)
  message(FATAL_ERROR "DependencyPrimerLinux.cmake should only be included on Linux/Unix")
endif()

message(STATUS "Configuring Linux platform support...")

# ######################################################################################################################
# Clang-Tidy Configuration
# ######################################################################################################################
if(CANOPY_ENABLE_CLANG_TIDY)
  find_program(CLANG_TIDY_EXE NAMES "clang-tidy" REQUIRED)

  if(CANOPY_ENABLE_CLANG_TIDY_FIX)
    set(CLANG_TIDY_COMMAND "${CLANG_TIDY_EXE}" -fix-errors -fix)
  else()
    set(CLANG_TIDY_COMMAND "${CLANG_TIDY_EXE}")
  endif()
else()
  set(CLANG_TIDY_COMMAND)
endif()

# ######################################################################################################################
# Platform Directories
# ######################################################################################################################
cmake_path(SET TEMP_DIR NORMALIZE "/tmp/")
cmake_path(SET RUNTIME_DIR NORMALIZE "/var/secretarium/runtime/")

# ######################################################################################################################
# Platform-Specific Defines
# ######################################################################################################################
list(APPEND CANOPY_SHARED_DEFINES TEMP_DIR="${TEMP_DIR}" RUNTIME_DIR="${RUNTIME_DIR}")

# ######################################################################################################################
# Build Type Configuration
# ######################################################################################################################
if(${BUILD_TYPE} STREQUAL "release")
  set(CMAKE_CXX_FLAGS_DEBUG "")
  set(CMAKE_C_FLAGS_DEBUG "")
  set(CANOPY_OPTIMIZER_FLAGS -O3)
  set(CANOPY_DEFINES ${CANOPY_SHARED_DEFINES} NDEBUG)
else()
  # Debug configuration
  set(EXTRA_COMPILE_OPTIONS ${CANOPY_DEBUG_ENCLAVE_OPTIONS})
  set(CMAKE_CXX_FLAGS_DEBUG ${CANOPY_DEBUG_COMPILE_FLAGS})
  set(CMAKE_C_FLAGS_DEBUG ${CMAKE_CXX_FLAGS_DEBUG})
  set(CANOPY_OPTIMIZER_FLAGS -O0)
  set(CANOPY_DEFINES ${CANOPY_SHARED_DEFINES} _DEBUG)
endif()

message("CMAKE_CXX_FLAGS_DEBUG [${CMAKE_CXX_FLAGS_DEBUG}]")
message("CANOPY_OPTIMIZER_FLAGS [${CANOPY_OPTIMIZER_FLAGS}]")

# ######################################################################################################################
# Shared Compile Options
# ######################################################################################################################
set(CANOPY_SHARED_COMPILE_OPTIONS
    -Wno-unknown-pragmas
    -Wno-deprecated-declarations
    -Wno-gnu-zero-variadic-macro-arguments
    ${EXTRA_COMPILE_OPTIONS}
    ${CANOPY_OPTIMIZER_FLAGS})

if(CANOPY_BUILD_COROUTINE)
  list(APPEND CANOPY_SHARED_COMPILE_OPTIONS # -fcoroutines  # Uncomment if needed for specific compilers
  )
endif()

# ######################################################################################################################
# Compiler-Specific Warning Flags
# ######################################################################################################################
message("CMAKE_CXX_COMPILER_ID ${CMAKE_CXX_COMPILER_ID}")

if(${CMAKE_CXX_COMPILER_ID} STREQUAL "Clang")
  set(CANOPY_CLANG_WARNS
      -Wc99-extensions
      -Wzero-length-array
      -Wflexible-array-extensions
      -Wpragma-pack-suspicious-include
      -Wshadow-field-in-constructor
      -Wno-gnu-zero-variadic-macro-arguments
      -Wno-implicit-exception-spec-mismatch
      # Extra checks
      -Wnon-virtual-dtor
      -Wdelete-non-virtual-dtor
      # Modern C++ best practices
      -Winconsistent-missing-override # Catch missing override keywords
      -Wimplicit-fallthrough # Require [[fallthrough]] in switch
      -Wunused-lambda-capture # Catch unused lambda captures
      -Wrange-loop-analysis # Catch inefficient range loops
      -Wconditional-uninitialized # Catch potentially uninitialized vars
      -Wmove # Catch suspicious std::move usage
      -Wunreachable-code # Catch unreachable code
      -Wcast-align # Warn about alignment-increasing casts
      -Wformat-security # Catch format string vulnerabilities
      -Wnull-dereference # Catch potential null dereferences
  )
else()
  # GCC
  set(CANOPY_CLANG_WARNS
      -Wno-variadic-macros
      -Wno-gnu-zero-variadic-macro-arguments
      -Wno-c++20-extensions
      # GCC equivalents where available
      -Wsuggest-override # GCC equivalent of -Winconsistent-missing-override
      -Wimplicit-fallthrough=5 # Highest level fallthrough warnings
      -Wunused # Catch unused variables/functions
      -Wformat-security # Format string security
  )
endif()

# ######################################################################################################################
# Warning Level Configurations
# ######################################################################################################################
set(CANOPY_WARN_BASELINE
    ${CANOPY_CLANG_WARNS}
    -Werror # convert warnings into errors
    -Wall
    -Wextra
    -Wno-variadic-macros # needed by yas
)

# Pedantic warnings - for maximum strictness (use in generator and pure code)
set(CANOPY_WARN_PEDANTIC
    -DCANOPY_WARN_PEDANTIC
    ${CANOPY_WARN_BASELINE}
    -Wpedantic # be pedantic
    # issue with fmt -Wsign-conversion # Warn on implicit sign conversions -Wconversion # Warn on implicit type
    # conversions
    -Wfloat-conversion # Warn on float conversions
    -Wdouble-promotion # Warn when float promoted to double
)

# Additional type safety warnings (optional, can be added selectively)
set(CANOPY_WARN_SIGN_CONVERSION -Wsign-conversion)
set(CANOPY_WARN_TYPE_SIZES -Wshorten-64-to-32 -Wsign-compare -Wshift-sign-overflow)

# Standard warnings for most code (tests, transports, etc.)
set(CANOPY_WARN_OK
    -DCANOPY_WARN_OK
    ${CANOPY_WARN_BASELINE}
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-sign-compare)

# ######################################################################################################################
# Sanitizer Configuration
# ######################################################################################################################
set(CANOPY_DEBUG_OPTIONS)

if(CANOPY_BUILD_TEST)
  if(CANOPY_DEBUG_ALL)
    set(CANOPY_DEBUG_LEAK ON)
    set(CANOPY_DEBUG_ADDRESS ON)
    set(CANOPY_DEBUG_THREAD OFF) # Cannot be used with leak sanitizer
    set(CANOPY_DEBUG_UNDEFINED ON)
  endif()

  if(CANOPY_DEBUG_ADDRESS)
    list(APPEND CANOPY_DEBUG_OPTIONS -fsanitize=address -fno-omit-frame-pointer)
  endif()

  if(CANOPY_DEBUG_THREAD)
    list(APPEND CANOPY_DEBUG_OPTIONS -fsanitize=thread -fno-omit-frame-pointer)
    set(CMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE PRE_TEST)
  endif()

  if(CANOPY_DEBUG_UNDEFINED)
    list(APPEND CANOPY_DEBUG_OPTIONS -fsanitize=undefined)
  endif()

  if(CANOPY_DEBUG_LEAK)
    if(CANOPY_DEBUG_ADDRESS)
      list(APPEND CANOPY_DEBUG_OPTIONS -fsanitize=leak -fno-omit-frame-pointer)
    else()
      list(APPEND CANOPY_DEBUG_OPTIONS -fsanitize=leak)
    endif()
  endif()

  if(CANOPY_DEBUG_MEMORY)
    if(CANOPY_DEBUG_ADDRESS)
      list(APPEND CANOPY_DEBUG_OPTIONS -fsanitize=leak -fno-omit-frame-pointer)
    else()
      list(APPEND CANOPY_DEBUG_OPTIONS -fsanitize=leak)
    endif()
  endif()
endif()

# ######################################################################################################################
# Link Options
# ######################################################################################################################
set(CANOPY_LINK_OPTIONS ${CANOPY_DEBUG_OPTIONS})
set(CANOPY_LINK_EXE_OPTIONS ${CANOPY_DEBUG_OPTIONS})
set(CANOPY_LINK_DYNAMIC_LIBRARY_OPTIONS ${CANOPY_LINK_OPTIONS} -fPIC)

# ######################################################################################################################
# Compile Options
# ######################################################################################################################
set(CANOPY_COMPILE_OPTIONS ${CANOPY_SHARED_COMPILE_OPTIONS} -Wno-trigraphs ${CANOPY_DEBUG_OPTIONS})

# ######################################################################################################################
# Executable Linker Flags (rpath for shared libraries)
# ######################################################################################################################
set(CMAKE_EXE_LINKER_FLAGS [[-Wl,-rpath,'$ORIGIN']])

# ######################################################################################################################
# Code Coverage Configuration
# ######################################################################################################################
if(CANOPY_ENABLE_COVERAGE)
  message("Enabling code coverage")
  # Using GCC gcov for coverage
  list(APPEND CANOPY_COMPILE_OPTIONS -fprofile-arcs -ftest-coverage)
  set(CMAKE_EXE_LINKER_FLAGS [[-Wl,-rpath,'$ORIGIN' -fprofile-arcs -ftest-coverage ]])
endif()

# ######################################################################################################################
# Final Library List
# ######################################################################################################################
set(CANOPY_LIBRARIES ${CANOPY_SHARED_LIBRARIES})

message(STATUS "Linux platform support configured successfully")
