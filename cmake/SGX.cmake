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
# SGX SDK bootstrap options
# ######################################################################################################################
# SGX.cmake owns high-level policy: - whether enclave support is enabled at all - whether a missing SDK may be
# bootstrapped from the local source submodule - Canopy-specific compile/link flags layered on top of the resolved SDK
#
# FindSGX.cmake remains the source of truth for SDK discovery details: - locating the installed SDK layout - resolving
# sgx_edger8r, sgx_sign, include dirs, and library dirs - defining the SGX helper functions/macros used by enclave
# targets
#
# The bootstrap path here only ensures an installed SDK exists before find_package(SGX REQUIRED) runs. It does not
# replace FindSGX.cmake.
option(CANOPY_BOOTSTRAP_SGX_SDK "Build and install the Intel SGX SDK from the local source submodule when missing" OFF)
set(CANOPY_SGX_SDK_INSTALL_PREFIX
    "${CMAKE_BINARY_DIR}/sgx-sdk"
    CACHE PATH "Install prefix root used when bootstrapping the Intel SGX SDK from submodule source")
set(CANOPY_SGX_BOOTSTRAP_CMAKE
    ""
    CACHE FILEPATH "Optional CMake executable to use only for SGX SDK bootstrap sub-builds")

function(canopy_resolve_existing_sgx_sdk out_var)
  set(candidate_paths)

  if(DEFINED SGX_DIR AND EXISTS "${SGX_DIR}")
    list(APPEND candidate_paths "${SGX_DIR}")
  endif()
  if(DEFINED SGX_ROOT AND EXISTS "${SGX_ROOT}")
    list(APPEND candidate_paths "${SGX_ROOT}")
  endif()
  if(DEFINED ENV{SGX_SDK} AND EXISTS "$ENV{SGX_SDK}")
    list(APPEND candidate_paths "$ENV{SGX_SDK}")
  endif()
  if(DEFINED ENV{SGX_DIR} AND EXISTS "$ENV{SGX_DIR}")
    list(APPEND candidate_paths "$ENV{SGX_DIR}")
  endif()
  if(DEFINED ENV{SGX_ROOT} AND EXISTS "$ENV{SGX_ROOT}")
    list(APPEND candidate_paths "$ENV{SGX_ROOT}")
  endif()

  if(WIN32)
    list(APPEND candidate_paths "C:/PROGRA~2/Intel/INTELS~1")
  else()
    list(APPEND candidate_paths "${CANOPY_SGX_SDK_INSTALL_PREFIX}/sgxsdk" "/opt/intel/sgxsdk")
  endif()

  foreach(candidate_path IN LISTS candidate_paths)
    if(EXISTS "${candidate_path}/include/sgx.h")
      set(${out_var}
          "${candidate_path}"
          PARENT_SCOPE)
      return()
    endif()
  endforeach()

  set(${out_var}
      ""
      PARENT_SCOPE)
endfunction()

function(canopy_bootstrap_sgx_sdk)
  if(WIN32)
    message(
      FATAL_ERROR "CANOPY_BOOTSTRAP_SGX_SDK is not supported on Windows. Set SGX_DIR to an installed Intel SGX SDK.")
  endif()

  set(sgx_source_dir "${CMAKE_CURRENT_LIST_DIR}/../submodules/confidential-computing.sgx")
  if(NOT IS_DIRECTORY "${sgx_source_dir}")
    message(FATAL_ERROR "SGX source submodule directory not found at ${sgx_source_dir}. "
                        "Enable GIT_SUBMODULE or populate submodules/confidential-computing.sgx before enclave builds.")
  endif()

  file(
    GLOB sgx_source_entries
    LIST_DIRECTORIES true
    RELATIVE "${sgx_source_dir}"
    "${sgx_source_dir}/*")
  list(LENGTH sgx_source_entries sgx_source_entry_count)
  if(sgx_source_entry_count EQUAL 0)
    message(FATAL_ERROR "submodules/confidential-computing.sgx exists but is empty. "
                        "Populate the submodule before enclave builds.")
  endif()

  find_program(CANOPY_MAKE_EXECUTABLE make)
  if(NOT CANOPY_MAKE_EXECUTABLE)
    message(FATAL_ERROR "Cannot bootstrap Intel SGX SDK because 'make' was not found.")
  endif()

  find_program(CANOPY_AUTORECONF_EXECUTABLE autoreconf)
  if(NOT CANOPY_AUTORECONF_EXECUTABLE)
    message(FATAL_ERROR "Cannot bootstrap Intel SGX SDK because 'autoreconf' was not found. "
                        "Install the autotools/autoconf prerequisite set or set SGX_DIR to an existing SDK.")
  endif()

  if(CANOPY_SGX_BOOTSTRAP_CMAKE)
    if(NOT EXISTS "${CANOPY_SGX_BOOTSTRAP_CMAKE}")
      message(
        FATAL_ERROR
          "CANOPY_SGX_BOOTSTRAP_CMAKE is set to '${CANOPY_SGX_BOOTSTRAP_CMAKE}', but that path does not exist.")
    endif()
    set(CANOPY_SYSTEM_CMAKE_EXECUTABLE "${CANOPY_SGX_BOOTSTRAP_CMAKE}")
  else()
    find_program(CANOPY_SYSTEM_CMAKE_EXECUTABLE cmake)
  endif()

  if(NOT CANOPY_SYSTEM_CMAKE_EXECUTABLE)
    message(FATAL_ERROR "Cannot bootstrap Intel SGX SDK because 'cmake' was not found.")
  endif()

  set(canopy_sgx_bootstrap_tools_dir "${CMAKE_BINARY_DIR}/sgx-bootstrap-tools")
  file(MAKE_DIRECTORY "${canopy_sgx_bootstrap_tools_dir}")
  set(canopy_sgx_bootstrap_cmake_wrapper "${canopy_sgx_bootstrap_tools_dir}/cmake")
  file(
    WRITE "${canopy_sgx_bootstrap_cmake_wrapper}"
    "#!/usr/bin/env bash\n"
    "if [ \"$1\" = \"-E\" ] || [ \"$1\" = \"--build\" ] || [ \"$1\" = \"--install\" ] || [ \"$1\" = \"--version\" ] || [ \"$1\" = \"--help\" ]; then\n"
    "  exec \"${CANOPY_SYSTEM_CMAKE_EXECUTABLE}\" \"$@\"\n"
    "fi\n"
    "exec \"${CANOPY_SYSTEM_CMAKE_EXECUTABLE}\" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_COMMAND=\"${canopy_sgx_bootstrap_cmake_wrapper}\" \"$@\"\n"
  )
  file(
    CHMOD
    "${canopy_sgx_bootstrap_cmake_wrapper}"
    PERMISSIONS
    OWNER_READ
    OWNER_WRITE
    OWNER_EXECUTE
    GROUP_READ
    GROUP_EXECUTE
    WORLD_READ
    WORLD_EXECUTE)

  if(DEFINED ENV{PATH})
    set(canopy_sgx_bootstrap_path "${canopy_sgx_bootstrap_tools_dir}:$ENV{PATH}")
  else()
    set(canopy_sgx_bootstrap_path "${canopy_sgx_bootstrap_tools_dir}")
  endif()

  message(STATUS "Bootstrapping Intel SGX SDK from submodules/confidential-computing.sgx")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "PATH=${canopy_sgx_bootstrap_path}" "CC=gcc" "CXX=g++" "CFLAGS=-std=gnu17"
            "CXXFLAGS=-std=gnu++17" "${CANOPY_MAKE_EXECUTABLE}" preparation
    WORKING_DIRECTORY "${sgx_source_dir}"
    RESULT_VARIABLE sgx_sdk_prep_result)

  if(NOT sgx_sdk_prep_result EQUAL 0)
    message(FATAL_ERROR "Failed to prepare Intel SGX SDK sources in ${sgx_source_dir}. "
                        "Install the SGX SDK prerequisites or inspect the submodule preparation step.")
  endif()

  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "PATH=${canopy_sgx_bootstrap_path}" "CC=gcc" "CXX=g++" "CFLAGS=-std=gnu17"
            "CXXFLAGS=-std=gnu++17" "${CANOPY_MAKE_EXECUTABLE}" sdk_install_pkg_no_mitigation USE_OPT_LIBS=1
    WORKING_DIRECTORY "${sgx_source_dir}"
    RESULT_VARIABLE sgx_sdk_build_result)

  if(NOT sgx_sdk_build_result EQUAL 0)
    message(FATAL_ERROR "Failed to build Intel SGX SDK from ${sgx_source_dir}. "
                        "Install the SGX SDK prerequisites or set SGX_DIR to an existing SDK.")
  endif()

  file(GLOB sgx_sdk_installers "${sgx_source_dir}/linux/installer/bin/sgx_linux_x64_sdk_*.bin")
  list(LENGTH sgx_sdk_installers sgx_sdk_installer_count)
  if(sgx_sdk_installer_count EQUAL 0)
    message(FATAL_ERROR "Intel SGX SDK build completed but no installer was produced under "
                        "${sgx_source_dir}/linux/installer/bin.")
  endif()

  list(GET sgx_sdk_installers 0 sgx_sdk_installer)
  file(MAKE_DIRECTORY "${CANOPY_SGX_SDK_INSTALL_PREFIX}")

  execute_process(
    COMMAND "${sgx_sdk_installer}" "--prefix=${CANOPY_SGX_SDK_INSTALL_PREFIX}"
    WORKING_DIRECTORY "${sgx_source_dir}"
    RESULT_VARIABLE sgx_sdk_install_result)

  if(NOT sgx_sdk_install_result EQUAL 0)
    message(FATAL_ERROR "Failed to install Intel SGX SDK into ${CANOPY_SGX_SDK_INSTALL_PREFIX}. "
                        "Set SGX_DIR to a valid SDK or inspect the submodule build environment.")
  endif()

  if(NOT EXISTS "${CANOPY_SGX_SDK_INSTALL_PREFIX}/sgxsdk/include/sgx.h")
    message(
      FATAL_ERROR "Intel SGX SDK installer completed but ${CANOPY_SGX_SDK_INSTALL_PREFIX}/sgxsdk was not created.")
  endif()

  set(SGX_DIR
      "${CANOPY_SGX_SDK_INSTALL_PREFIX}/sgxsdk"
      CACHE PATH "Intel SGX SDK directory" FORCE)
  message(STATUS "Intel SGX SDK installed to ${SGX_DIR}")
endfunction()

# Resolve or create an installed SDK location first, then defer the actual SGX tool/library discovery to FindSGX.cmake
# below.
canopy_resolve_existing_sgx_sdk(CANOPY_EXISTING_SGX_SDK)
if(NOT CANOPY_EXISTING_SGX_SDK AND CANOPY_BOOTSTRAP_SGX_SDK)
  canopy_bootstrap_sgx_sdk()
elseif(CANOPY_EXISTING_SGX_SDK)
  set(SGX_DIR
      "${CANOPY_EXISTING_SGX_SDK}"
      CACHE PATH "Intel SGX SDK directory" FORCE)
else()
  if(SGX_HW)
    message(
      FATAL_ERROR
        "Intel SGX SDK not found. Set SGX_DIR to an installed SDK, or configure a simulation build "
        "(for example Debug_SGX_Sim) with CANOPY_BOOTSTRAP_SGX_SDK=ON if you want CMake to build "
        "the SDK from submodules/confidential-computing.sgx.")
  else()
    message(
      FATAL_ERROR
        "Intel SGX SDK not found. For simulation builds on this machine, reconfigure with "
        "CANOPY_BOOTSTRAP_SGX_SDK=ON or use the Debug_SGX_Sim/Release_SGX_Sim presets that enable "
        "SDK bootstrap. On machines with an existing SDK, set SGX_DIR to that install path.")
  endif()
endif()

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
  set(CANOPY_SHARED_ENCLAVE_DEFINES ${CANOPY_SHARED_DEFINES} FOR_SGX ${CANOPY_ENCLAVE_MEMLEAK_DEFINES})

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
    set(CANOPY_ENCLAVE_DEFINES
        ${CANOPY_SHARED_ENCLAVE_DEFINES}
        SGX_DEBUG=1
        VERBOSE=2
        _DEBUG
        ${CANOPY_ENCLAVE_MEMLEAK_DEFINES})

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

  # Shared enclave defines (Linux). GCC reaches the SGX SDK's libc++ configuration through libcxx/__config ->
  # libcxx/__sgx, so we must not predefine __LIBCPP_SGX there or the SDK's header becomes a no-op. Clang does not take
  # that path in the SGX SDK headers, so we still provide the SGX libc++ compatibility defines explicitly for Clang.
  set(CANOPY_SHARED_ENCLAVE_DEFINES
      FOR_SGX
      ${CANOPY_SHARED_DEFINES}
      CLEAN_LIBC
      ENCLAVE_STATUS=sgx_status_t
      ENCLAVE_OK=SGX_SUCCESS
      DISALLOW_BAD_JUMPS
      _LIBCPP_DISABLE_AVAILABILITY
      __THROW=)

  if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    list(
      APPEND
      CANOPY_SHARED_ENCLAVE_DEFINES
      _LIBCPP_SGX_CONFIG
      _LIBCPP_HAS_NO_THREADS
      _LIBCPP_HAS_NO_STDIN
      _LIBCPP_HAS_NO_STDOUT
      _LIBCPP_HAS_NO_GLOBAL_FILESYSTEM_NAMESPACE
      _LIBCPP_SGX_NO_IOSTREAMS
      _LIBCPP_TYPE_VIS_ONLY=
      __LIBCPP_SGX
      _LIBCPP_SGX_HAS_CXX_ATOMIC
      _LIBCPP_ATOMIC_FLAG_TYPE=bool)
  endif()

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
    set(CANOPY_ENCLAVE_DEFINES ${CANOPY_SHARED_ENCLAVE_DEFINES} SGX_DEBUG=1 _DEBUG ${CANOPY_ENCLAVE_MEMLEAK_DEFINES})
  endif()

  # Enclave compile options (Linux) -nostdinc / -nostdinc++ are mandatory: they prevent the SGX libcxx
  # __threading_support from pulling in real system headers (sched.h, semaphore.h) which are incompatible with the
  # enclave ABI. The SGX SDK provides its own replacements via the explicit -I paths. -fvisibility=hidden / -fpie /
  # -fstack-protector-strong are required by the SGX linker. -Wno-variadic-macros must come after
  # ${CANOPY_SHARED_COMPILE_OPTIONS} because -Wpedantic re-enables it; the SGX SDK's own sgx_defs.h uses named variadic
  # macros.
  set(CANOPY_ENCLAVE_COMPILE_OPTIONS
      ${CANOPY_SHARED_COMPILE_OPTIONS}
      -nostdinc
      -nostdinc++
      -fvisibility=hidden
      -fpie
      -fstack-protector-strong
      -Wno-c++17-extensions
      -ffunction-sections
      -fdata-sections
      -Wno-implicit-exception-spec-mismatch
      -Wno-variadic-macros)

  # Enclave-specific pedantic warning set: identical to CANOPY_WARN_PEDANTIC but with -Wno-variadic-macros appended last
  # so it overrides the -Wpedantic re-enablement. SGX SDK headers (sgx_defs.h) use named variadic macros which
  # -Wpedantic flags. Using a separate variable avoids cmake list-append ordering issues. SHELL: prefix prevents cmake
  # from deduplicating this flag against the earlier -Wno-variadic-macros in CANOPY_ENCLAVE_COMPILE_OPTIONS, ensuring it
  # lands after -Wpedantic (which re-enables the warning) in the final compile command.
  set(CANOPY_WARN_PEDANTIC_ENCLAVE ${CANOPY_WARN_PEDANTIC} "SHELL:-Wno-variadic-macros")
  set(CANOPY_WARN_OK_ENCLAVE ${CANOPY_WARN_OK} "SHELL:-Wno-variadic-macros")
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    list(
      APPEND
      CANOPY_WARN_PEDANTIC_ENCLAVE
      "SHELL:-Wno-pedantic"
      "SHELL:-Wno-suggest-override"
      "SHELL:-Wno-changes-meaning"
      "SHELL:-Wno-dangling-reference")
    list(
      APPEND
      CANOPY_WARN_OK_ENCLAVE
      "SHELL:-Wno-suggest-override"
      "SHELL:-Wno-changes-meaning"
      "SHELL:-Wno-dangling-reference")
  endif()

  # Resolve the installed SDK layout, tools, libraries, helper functions, and SGX-specific CMake abstractions via
  # FindSGX.cmake.
  find_package(SGX REQUIRED)

  # Update link options with the resolved SGX SDK library path
  set(CANOPY_LINK_OPTIONS -L${SGX_LIBRARY_PATH} -lsgx_tcrypto ${CANOPY_DEBUG_OPTIONS})

  set(CANOPY_SGX_HOST_EXE_LINK_OPTIONS)
  if(EXISTS "${SGX_LIBRARY_PATH}/libsgx_dcap_quoteverify.so")
    list(APPEND CANOPY_SGX_HOST_EXE_LINK_OPTIONS -lsgx_dcap_quoteverify)
  endif()
  if(EXISTS "${SGX_LIBRARY_PATH}/libsgx_dcap_ql.so")
    list(APPEND CANOPY_SGX_HOST_EXE_LINK_OPTIONS -lsgx_dcap_ql)
  endif()
  set(CANOPY_LINK_EXE_OPTIONS ${CANOPY_SGX_HOST_EXE_LINK_OPTIONS} ${CANOPY_DEBUG_OPTIONS})

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
