#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.

   SGX Enclave Configuration for Canopy

   This file is automatically included when CANOPY_BUILD_ENCLAVE=ON.
   It provides all SGX-specific compilation flags, defines, and settings
   needed for building Intel SGX enclaves.
]]

cmake_minimum_required(VERSION 3.24)

if(NOT CANOPY_BUILD_ENCLAVE)
  message(FATAL_ERROR "DependencyPrimerSGX.cmake should only be included when CANOPY_BUILD_ENCLAVE=ON")
endif()

message(STATUS "Configuring SGX enclave support...")

# ######################################################################################################################
# SGX-specific options
# ######################################################################################################################
option(CANOPY_DEBUG_ENCLAVE_MEMLEAK "detect memory leaks in enclaves" OFF)

# ######################################################################################################################
# SGX build messages
# ######################################################################################################################
message("SGX_MODE ${SGX_MODE}")
message("SGX_HW ${SGX_HW}")
message("SGX_KEY ${SGX_KEY}")
message("CANOPY_AWAIT_ATTACH_ON_ENCLAVE_ERRORS ${CANOPY_AWAIT_ATTACH_ON_ENCLAVE_ERRORS}")
message("CANOPY_DEBUG_ENCLAVE_MEMLEAK ${CANOPY_DEBUG_ENCLAVE_MEMLEAK}")

# ######################################################################################################################
# SGX hardware vs simulation defines
# ######################################################################################################################
if(${SGX_HW})
  set(SGX_HW_OR_SIM_DEFINE SGX_HW)
else()
  set(SGX_HW_OR_SIM_DEFINE SGX_SIM)
endif()

# ######################################################################################################################
# Enclave flag for defines
# ######################################################################################################################
set(CANOPY_BUILD_ENCLAVE_FLAG CANOPY_BUILD_ENCLAVE)

# ######################################################################################################################
# Enclave format library
# ######################################################################################################################
set(CANOPY_ENCLAVE_FMT_LIB fmt::fmt-header-only)

# ######################################################################################################################
# Platform-specific SGX configuration
# ######################################################################################################################
if(WIN32)
  # ####################################################################################################################
  # Windows SGX Configuration
  # ####################################################################################################################
  find_package(SGX REQUIRED)

  if(CANOPY_DEBUG_ENCLAVE_MEMLEAK)
    set(CANOPY_ENCLAVE_MEMLEAK_LINK_FLAGS /IGNORE:4006 /IGNORE:4088 /FORCE:MULTIPLE /WX:NO)
    set(CANOPY_ENCLAVE_MEMLEAK_DEFINES MEMLEAK_CHECK)
  endif()

  # Shared enclave defines (Windows)
  set(CANOPY_SHARED_ENCLAVE_DEFINES ${CANOPY_SHARED_DEFINES} _IN_ENCLAVE ${CANOPY_ENCLAVE_MEMLEAK_DEFINES})

  # Enclave compile options (Windows)
  set(CANOPY_SHARED_ENCLAVE_COMPILE_OPTIONS ${CANOPY_SHARED_COMPILE_OPTIONS} /d2FH4- /Qspectre)

  # Enclave link options (Windows)
  set(CANOPY_SHARED_ENCLAVE_LINK_OPTIONS ${CANOPY_LINK_OPTIONS})

  if(CMAKE_BUILD_TYPE STREQUAL "Release")
    if(${SGX_MODE} STREQUAL "release")
      set(CANOPY_ENCLAVE_DEFINES ${CANOPY_SHARED_ENCLAVE_DEFINES} NDEBUG)
    else()
      # Prerelease with debug enclave flag
      set(CANOPY_ENCLAVE_DEFINES ${CANOPY_SHARED_ENCLAVE_DEFINES} NDEBUG EDEBUG # sets SGX_DEBUG_FLAG to 1
      )
    endif()

    message("Note: /GL needs to be re-enabled for performance reasons in release enclave builds")
    set(CANOPY_ENCLAVE_COMPILE_OPTIONS
        ${CANOPY_SHARED_ENCLAVE_COMPILE_OPTIONS}
        # /GL  # Disabled for now
        /MT
        /O2
        /Oi
        /Ob2)
    set(CANOPY_ENCLAVE_LINK_OPTIONS ${CANOPY_SHARED_ENCLAVE_LINK_OPTIONS} /INCREMENTAL:NO
                                    ${CANOPY_ENCLAVE_MEMLEAK_LINK_FLAGS} /DEBUG)
  else()
    # Debug configuration
    set(CANOPY_ENCLAVE_DEFINES ${CANOPY_SHARED_ENCLAVE_DEFINES} VERBOSE=2 _DEBUG ${CANOPY_ENCLAVE_MEMLEAK_DEFINES})

    set(CANOPY_ENCLAVE_COMPILE_OPTIONS ${CANOPY_SHARED_ENCLAVE_COMPILE_OPTIONS} /MDd /Od /Ob0)

    set(CANOPY_ENCLAVE_LINK_OPTIONS
        ${CANOPY_SHARED_ENCLAVE_LINK_OPTIONS}
        /IGNORE:4099
        /IGNORE:4204
        /IGNORE:4217
        /DEBUG
        /INCREMENTAL:NO
        ${CANOPY_ENCLAVE_MEMLEAK_LINK_FLAGS})
  endif()

else()
  # ####################################################################################################################
  # Linux SGX Configuration
  # ####################################################################################################################
  if(CANOPY_DEBUG_ENCLAVE_MEMLEAK)
    set(CANOPY_ENCLAVE_MEMLEAK_DEFINES MEMLEAK_CHECK)
  endif()

  # Shared enclave defines (Linux)
  set(CANOPY_SHARED_ENCLAVE_DEFINES
      _IN_ENCLAVE
      ${CANOPY_SHARED_DEFINES}
      CLEAN_LIBC
      ENCLAVE_STATUS=sgx_status_t
      ENCLAVE_OK=SGX_SUCCESS
      DISALLOW_BAD_JUMPS)

  if(CMAKE_BUILD_TYPE STREQUAL "Release")
    if(${SGX_MODE} STREQUAL "release")
      set(CANOPY_ENCLAVE_DEFINES ${CANOPY_SHARED_ENCLAVE_DEFINES} NDEBUG ${CANOPY_ENCLAVE_MEMLEAK_DEFINES})
    else()
      # Prerelease
      set(CANOPY_ENCLAVE_DEFINES ${CANOPY_SHARED_ENCLAVE_DEFINES} NDEBUG EDEBUG # sets SGX_DEBUG_FLAG to 1
                                 ${CANOPY_ENCLAVE_MEMLEAK_DEFINES})
    endif()
  else()
    # Debug configuration
    set(CANOPY_ENCLAVE_DEFINES ${CANOPY_SHARED_ENCLAVE_DEFINES} _DEBUG ${CANOPY_ENCLAVE_MEMLEAK_DEFINES})
  endif()

  # Enclave compile options (Linux)
  set(CANOPY_ENCLAVE_COMPILE_OPTIONS
      ${CANOPY_SHARED_COMPILE_OPTIONS}
      -Wno-c++17-extensions
      -ffunction-sections
      -fdata-sections
      -Wno-implicit-exception-spec-mismatch)

  # Update link options with SGX libraries
  set(CANOPY_LINK_OPTIONS -L/opt/intel/sgxsdk/lib64 -lsgx_tcrypto ${CANOPY_DEBUG_OPTIONS})
  set(CANOPY_LINK_EXE_OPTIONS -lsgx_dcap_quoteverify -lsgx_dcap_ql ${CANOPY_DEBUG_OPTIONS})

  # Find SGX package
  find_package(SGX REQUIRED)

  # Check if SGX SDK has debug information
  if(SGX_SDK_CONTAINS_DEBUG_INFORMATION)
    list(APPEND CANOPY_ENCLAVE_DEFINES SGX_SDK_CONTAINS_DEBUG_INFORMATION)
  endif()

  # Enclave include directories
  set(CANOPY_ENCLAVE_LIBC_INCLUDES ${SGX_INCLUDE_DIR} ${SGX_TLIBC_INCLUDE_DIR})
  set(CANOPY_ENCLAVE_LIBCXX_INCLUDES ${CANOPY_ENCLAVE_LIBC_INCLUDES} ${SGX_LIBCXX_INCLUDE_DIR}
                                     ${SGX_LIBSTDCXX_INCLUDE_DIR})
endif()

# ######################################################################################################################
# Common SGX includes and libraries
# ######################################################################################################################
set(CANOPY_INCLUDES ${SGX_INCLUDE_DIR})

# Add SGX libraries to CANOPY_LIBRARIES
if(WIN32)
  if(${SGX_HW})
    list(
      APPEND
      CANOPY_LIBRARIES
      sgx_tcrypto.lib
      sgx_uae_service.lib
      sgx_capable.lib
      sgx_urts.lib)
  else()
    list(
      APPEND
      CANOPY_LIBRARIES
      sgx_tcrypto.lib
      sgx_uae_service_sim.lib
      sgx_capable.lib
      sgx_urts_sim.lib)
  endif()
else()
  list(
    APPEND
    CANOPY_LIBRARIES
    ${SGX_USVC_LIB}
    sgx_capable
    ${SGX_URTS_LIB})
endif()

# ######################################################################################################################
# Output configuration summary
# ######################################################################################################################
message("CANOPY_ENCLAVE_DEFINES ${CANOPY_ENCLAVE_DEFINES}")
message("CANOPY_ENCLAVE_COMPILE_OPTIONS ${CANOPY_ENCLAVE_COMPILE_OPTIONS}")
message("CANOPY_ENCLAVE_LINK_OPTIONS ${CANOPY_ENCLAVE_LINK_OPTIONS}")
message(STATUS "SGX enclave support configured successfully")
