#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.

   Windows Platform Configuration for Canopy

   This file is automatically included on Windows platforms.
   It provides Windows-specific compilation flags, defines, and settings.
]]

cmake_minimum_required(VERSION 3.24)

if(NOT WIN32)
  message(FATAL_ERROR "DependencyPrimerWindows.cmake should only be included on Windows")
endif()

message(STATUS "Configuring Windows platform support...")

# ######################################################################################################################
# Clear Default Windows Libraries and Flags
# ######################################################################################################################
# We need explicit control over libraries and flags for proper enclave support
set(CMAKE_C_STANDARD_LIBRARIES
    ""
    CACHE STRING "override default windows libraries" FORCE)
set(CMAKE_C_COMPILE_OBJECT
    ""
    CACHE STRING "override default windows libraries" FORCE)
set(CMAKE_C_ARCHIVE_FINISH
    ""
    CACHE STRING "override default windows libraries" FORCE)
set(CMAKE_C_ARCHIVE_CREATE
    ""
    CACHE STRING "override default windows libraries" FORCE)
set(CMAKE_CXX_STANDARD_LIBRARIES
    ""
    CACHE STRING "override default windows libraries" FORCE)
set(CMAKE_CXX_FLAGS
    ""
    CACHE STRING "override default windows flags" FORCE)
set(CMAKE_C_FLAGS
    ""
    CACHE STRING "override default windows flags" FORCE)
set(CMAKE_CXX_FLAGS_DEBUG
    ""
    CACHE STRING "override default windows flags" FORCE)
set(CMAKE_C_FLAGS_DEBUG
    ""
    CACHE STRING "override default windows flags" FORCE)
set(CMAKE_CXX_FLAGS_RELEASE
    ""
    CACHE STRING "override default windows flags" FORCE)
set(CMAKE_C_FLAGS_RELEASE
    ""
    CACHE STRING "override default windows flags" FORCE)

set(CMAKE_MODULE_LINKER_FLAGS
    ""
    CACHE STRING "override default windows flags" FORCE)
set(CMAKE_MODULE_LINKER_FLAGS_DEBUG
    ""
    CACHE STRING "override default windows flags" FORCE)
set(CMAKE_MODULE_LINKER_FLAGS_MINSIZEREL
    ""
    CACHE STRING "override default windows flags" FORCE)
set(CMAKE_MODULE_LINKER_FLAGS_RELEASE
    ""
    CACHE STRING "override default windows flags" FORCE)
set(CMAKE_MODULE_LINKER_FLAGS_RELWITHDEBINFO
    ""
    CACHE STRING "override default windows flags" FORCE)
set(CMAKE_SHARED_LINKER_FLAGS
    ""
    CACHE STRING "override default windows flags" FORCE)
set(CMAKE_SHARED_LINKER_FLAGS_DEBUG
    ""
    CACHE STRING "override default windows flags" FORCE)
set(CMAKE_SHARED_LINKER_FLAGS_MINSIZEREL
    ""
    CACHE STRING "override default windows flags" FORCE)
set(CMAKE_SHARED_LINKER_FLAGS_RELEASE
    ""
    CACHE STRING "override default windows flags" FORCE)
set(CMAKE_SHARED_LINKER_FLAGS_RELWITHDEBINFO
    ""
    CACHE STRING "override default windows flags" FORCE)
set(CMAKE_EXE_LINKER_FLAGS
    ""
    CACHE STRING "override default windows flags" FORCE)
set(CMAKE_EXE_LINKER_FLAGS_DEBUG
    ""
    CACHE STRING "override default windows flags" FORCE)
set(CMAKE_EXE_LINKER_FLAGS_MINSIZEREL
    ""
    CACHE STRING "override default windows flags" FORCE)
set(CMAKE_EXE_LINKER_FLAGS_RELEASE
    ""
    CACHE STRING "override default windows flags" FORCE)
set(CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO
    ""
    CACHE STRING "override default windows flags" FORCE)

# ######################################################################################################################
# Warning Flags
# ######################################################################################################################
set(CANOPY_WARNING_FLAG /W3 /WX /w34265 /wd4996)

# Warning level variables (empty on Windows - using CANOPY_WARNING_FLAG instead)
set(CANOPY_WARN_PEDANTIC)
set(CANOPY_WARN_OK)

# ######################################################################################################################
# Platform Directories
# ######################################################################################################################
cmake_path(SET TEMP_DIR NORMALIZE "c:/temp/")
cmake_path(SET RUNTIME_DIR NORMALIZE "c:/Secretarium/Runtime/")

# ######################################################################################################################
# Platform-Specific Defines
# ######################################################################################################################
# WIN32_LEAN_AND_MEAN prevents conflicts between winsock.h and winsock2.h
list(
  APPEND
  CANOPY_SHARED_DEFINES
  TEMP_DIR="${TEMP_DIR}"
  RUNTIME_DIR="${RUNTIME_DIR}"
  WIN32
  _WINDOWS
  WIN32_LEAN_AND_MEAN)

# ######################################################################################################################
# Shared Compile Options
# ######################################################################################################################
set(CANOPY_SHARED_COMPILE_OPTIONS
    ${CANOPY_WARNING_FLAG}
    /bigobj
    /diagnostics:classic
    /EHsc
    /errorReport:prompt
    /FC
    /fp:precise
    /Gd
    /Gy
    /MP
    /nologo
    /sdl
    /Zc:forScope
    /Zc:inline
    /Zc:rvalueCast
    /Zc:wchar_t
    /Zc:__cplusplus
    /Zi
    /wd4996 # allow deprecated functions
)

set(CANOPY_DEBUG_OPTIONS)
set(CANOPY_SHARED_COMPILE_OPTIONS ${CANOPY_SHARED_COMPILE_OPTIONS} ${CANOPY_DEBUG_OPTIONS})

# ######################################################################################################################
# Link Options
# ######################################################################################################################
set(CANOPY_LINK_OPTIONS /MACHINE:x64 /WX)
set(CANOPY_SHARED_LINK_OPTIONS ${CANOPY_LINK_OPTIONS} ${CANOPY_DEBUG_OPTIONS})

# ######################################################################################################################
# System Libraries
# ######################################################################################################################
set(CANOPY_SHARED_LIBRARIES
    advapi32.lib
    comdlg32.lib
    gdi32.lib
    kernel32.lib
    ole32.lib
    oleaut32.lib
    shell32.lib
    user32.lib
    uuid.lib
    winspool.lib
    Crypt32.lib)

# ######################################################################################################################
# Build Type Configuration
# ######################################################################################################################
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  set(CANOPY_DEFINES ${CANOPY_SHARED_DEFINES} NDEBUG)

  set(CANOPY_COMPILE_OPTIONS
      ${CANOPY_SHARED_COMPILE_OPTIONS}
      /GL
      /MD
      /O2
      /Oi
      /Ob2)

  set(CANOPY_LINK_OPTIONS ${CANOPY_SHARED_LINK_OPTIONS} /INCREMENTAL:NO /DEBUG)
  set(CANOPY_LINK_DYNAMIC_LIBRARY_OPTIONS ${CANOPY_LINK_OPTIONS})
  set(CANOPY_LINK_EXE_OPTIONS ${CANOPY_LINK_OPTIONS} /IGNORE:4099 /IGNORE:4098)
else()
  # Debug configuration
  set(CANOPY_DEFINES ${CANOPY_SHARED_DEFINES} _DEBUG)

  set(CANOPY_COMPILE_OPTIONS
      ${CANOPY_SHARED_COMPILE_OPTIONS}
      /MDd
      /Od
      /Ob0
      /RTC1)

  set(CANOPY_LINK_OPTIONS ${CANOPY_SHARED_LINK_OPTIONS} /DEBUG /INCREMENTAL)
  set(CANOPY_LINK_DYNAMIC_LIBRARY_OPTIONS ${CANOPY_LINK_OPTIONS})
  set(CANOPY_LINK_EXE_OPTIONS
      ${CANOPY_LINK_OPTIONS}
      /IGNORE:4099
      /IGNORE:4098
      /IGNORE:4204
      /IGNORE:4203)
endif()

# ######################################################################################################################
# Final Library List
# ######################################################################################################################
set(CANOPY_LIBRARIES ${CANOPY_SHARED_LIBRARIES})

message(STATUS "Windows platform support configured successfully")
