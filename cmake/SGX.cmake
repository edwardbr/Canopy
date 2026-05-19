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
option(CANOPY_SGX_DETERMINISTIC_BUILD "Apply deterministic/reproducible-build settings to SGX enclave builds" ON)
set(CANOPY_SGX_SOURCE_DATE_EPOCH
    ""
    CACHE STRING "Optional SOURCE_DATE_EPOCH value passed to SGX helper tools during deterministic enclave builds")

set(CANOPY_SGX_DETERMINISTIC_ENV)
set(CANOPY_SGX_DETERMINISTIC_COMPILE_OPTIONS)
set(CANOPY_SGX_DETERMINISTIC_LINK_OPTIONS)
if(CANOPY_SGX_DETERMINISTIC_BUILD)
  # These environment variables are intentionally small and non-invasive. LC_ALL stabilises tool output ordering,
  # ZERO_AR_DATE is honoured by archive tools that still support the historical deterministic-archive switch, and
  # SOURCE_DATE_EPOCH is propagated only when the caller pins it explicitly or inherited it from the outer build
  # environment.
  list(APPEND CANOPY_SGX_DETERMINISTIC_ENV "LC_ALL=C" "ZERO_AR_DATE=1")
  if(NOT "${CANOPY_SGX_SOURCE_DATE_EPOCH}" STREQUAL "")
    list(APPEND CANOPY_SGX_DETERMINISTIC_ENV "SOURCE_DATE_EPOCH=${CANOPY_SGX_SOURCE_DATE_EPOCH}")
  elseif(DEFINED ENV{SOURCE_DATE_EPOCH} AND NOT "$ENV{SOURCE_DATE_EPOCH}" STREQUAL "")
    list(APPEND CANOPY_SGX_DETERMINISTIC_ENV "SOURCE_DATE_EPOCH=$ENV{SOURCE_DATE_EPOCH}")
  endif()

  if(NOT WIN32 AND (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU"))
    # Keep absolute source/build paths out of debug sections and macro expansions inside measured enclave objects.
    # -Wdate-time is valuable here because the top-level warning policy turns warnings into errors for normal builds:
    # enclave code that uses __DATE__, __TIME__, or __TIMESTAMP__ will fail instead of quietly changing MRENCLAVE.
    list(
      APPEND
      CANOPY_SGX_DETERMINISTIC_COMPILE_OPTIONS
      "-ffile-prefix-map=${CMAKE_SOURCE_DIR}=."
      "-ffile-prefix-map=${CMAKE_BINARY_DIR}=./_build"
      "-fdebug-prefix-map=${CMAKE_SOURCE_DIR}=."
      "-fdebug-prefix-map=${CMAKE_BINARY_DIR}=./_build"
      "-fmacro-prefix-map=${CMAKE_SOURCE_DIR}=."
      "-fmacro-prefix-map=${CMAKE_BINARY_DIR}=./_build"
      -Wdate-time)
    if(NOT WIN32)
      list(APPEND CANOPY_SGX_DETERMINISTIC_LINK_OPTIONS "LINKER:--build-id=none")
    endif()
  elseif(MSVC)
    list(
      APPEND
      CANOPY_SGX_DETERMINISTIC_COMPILE_OPTIONS
      /Brepro
      "/pathmap:${CMAKE_SOURCE_DIR}=."
      "/pathmap:${CMAKE_BINARY_DIR}=.\\_build")
  endif()
endif()

# ######################################################################################################################
# SGX build messages
# ######################################################################################################################
message("SGX_MODE ${SGX_MODE}")
message("SGX_HW ${SGX_HW}")
message("SGX_KEY ${SGX_KEY}")
message("CANOPY_AWAIT_ATTACH_ON_ENCLAVE_ERRORS ${CANOPY_AWAIT_ATTACH_ON_ENCLAVE_ERRORS}")
message("CANOPY_DEBUG_ENCLAVE_MEMLEAK ${CANOPY_DEBUG_ENCLAVE_MEMLEAK}")
message("CANOPY_SGX_DETERMINISTIC_BUILD ${CANOPY_SGX_DETERMINISTIC_BUILD}")
message("CANOPY_SGX_SOURCE_DATE_EPOCH ${CANOPY_SGX_SOURCE_DATE_EPOCH}")

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
option(CANOPY_SGX_BOOTSTRAP_UPDATE_SUBMODULES
       "Run the Intel SGX SDK preparation step, which updates nested SGX SDK submodules" ON)
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
  list(REMOVE_ITEM sgx_source_entries ".git")
  list(LENGTH sgx_source_entries sgx_source_entry_count)
  if(sgx_source_entry_count EQUAL 0)
    message(FATAL_ERROR "submodules/confidential-computing.sgx exists but is empty. "
                        "Populate the submodule before enclave builds.")
  endif()
  if(NOT EXISTS "${sgx_source_dir}/Makefile")
    message(FATAL_ERROR "submodules/confidential-computing.sgx does not contain the expected Intel SGX SDK Makefile. "
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

  file(GLOB sgx_sdk_installers "${sgx_source_dir}/linux/installer/bin/sgx_linux_x64_sdk_*.bin")
  list(LENGTH sgx_sdk_installers sgx_sdk_installer_count)

  if(sgx_sdk_installer_count EQUAL 0)
    set(canopy_sgx_prepared_markers
        "${sgx_source_dir}/external/ippcp_internal/lib/linux/intel64" "${sgx_source_dir}/external/cbor/sgx_libcbor"
        "${sgx_source_dir}/external/protobuf/protobuf_code/third_party/abseil-cpp"
        "${sgx_source_dir}/external/dcap_source/external/jwt-cpp")
    set(canopy_sgx_sources_prepared TRUE)
    foreach(canopy_sgx_prepared_marker IN LISTS canopy_sgx_prepared_markers)
      if(NOT EXISTS "${canopy_sgx_prepared_marker}")
        set(canopy_sgx_sources_prepared FALSE)
      endif()
    endforeach()

    if(CANOPY_SGX_BOOTSTRAP_UPDATE_SUBMODULES)
      message(STATUS "Bootstrapping Intel SGX SDK from submodules/confidential-computing.sgx")
      execute_process(
        COMMAND
          "${CMAKE_COMMAND}" -E env ${CANOPY_SGX_DETERMINISTIC_ENV} "PATH=${canopy_sgx_bootstrap_path}" "CC=gcc"
          "CXX=g++" "CFLAGS=-std=gnu17" "CXXFLAGS=-std=gnu++17" "CMAKE_POLICY_VERSION_MINIMUM=3.5"
          "${CANOPY_MAKE_EXECUTABLE}" preparation
        WORKING_DIRECTORY "${sgx_source_dir}"
        RESULT_VARIABLE sgx_sdk_prep_result)

      if(NOT sgx_sdk_prep_result EQUAL 0)
        message(FATAL_ERROR "Failed to prepare Intel SGX SDK sources in ${sgx_source_dir}. "
                            "Install the SGX SDK prerequisites or inspect the submodule preparation step.")
      endif()
    elseif(canopy_sgx_sources_prepared)
      message(STATUS "Skipping Intel SGX SDK source preparation because CANOPY_SGX_BOOTSTRAP_UPDATE_SUBMODULES is OFF "
                     "and the prepared source markers are present.")
    else()
      message(
        FATAL_ERROR
          "CANOPY_SGX_BOOTSTRAP_UPDATE_SUBMODULES is OFF, but no SGX SDK installer exists and "
          "the SGX source tree does not look prepared. Enable CANOPY_SGX_BOOTSTRAP_UPDATE_SUBMODULES "
          "for the first bootstrap, or populate submodules/confidential-computing.sgx manually.")
    endif()

    execute_process(
      COMMAND
        "${CMAKE_COMMAND}" -E env ${CANOPY_SGX_DETERMINISTIC_ENV} "PATH=${canopy_sgx_bootstrap_path}" "CC=gcc"
        "CXX=g++" "CFLAGS=-std=gnu17" "CXXFLAGS=-std=gnu++17" "CMAKE_POLICY_VERSION_MINIMUM=3.5"
        "${CANOPY_MAKE_EXECUTABLE}"
        sdk_install_pkg_no_mitigation USE_OPT_LIBS=1
      WORKING_DIRECTORY "${sgx_source_dir}"
      RESULT_VARIABLE sgx_sdk_build_result)

    if(NOT sgx_sdk_build_result EQUAL 0)
      message(FATAL_ERROR "Failed to build Intel SGX SDK from ${sgx_source_dir}. "
                          "Install the SGX SDK prerequisites or set SGX_DIR to an existing SDK.")
    endif()

    file(GLOB sgx_sdk_installers "${sgx_source_dir}/linux/installer/bin/sgx_linux_x64_sdk_*.bin")
    list(LENGTH sgx_sdk_installers sgx_sdk_installer_count)
  else()
    message(STATUS "Using existing Intel SGX SDK installer from submodules/confidential-computing.sgx")
  endif()

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

# ######################################################################################################################
# SGXSSL bootstrap and link support
# ######################################################################################################################
# SGXSSL is used by enclave code that needs OpenSSL-compatible APIs. It is not a normal CMake subproject: Intel ships a
# prepare_sgxssl.sh script that creates an SGX-compatible OpenSSL package under a build directory. Keep that setup here
# rather than in individual consumers so future SGX framework components can all share the same include directories,
# archive names, bootstrap target, and whole-archive handling.
function(canopy_add_imported_enclave_archive target archive)
  if(NOT TARGET ${target})
    add_library(${target} STATIC IMPORTED GLOBAL)
  endif()
  set_target_properties(${target} PROPERTIES IMPORTED_LOCATION "${archive}")
endfunction()

function(canopy_configure_sgxssl_for_enclave)
  set(CANOPY_SGXSSL_HEADERS_READY
      FALSE
      PARENT_SCOPE)
  set(CANOPY_SGXSSL_IMPL_LIBS_FOUND
      FALSE
      PARENT_SCOPE)
  set(CANOPY_SGXSSL_ENCLAVE_LINK_LIBRARIES
      ""
      PARENT_SCOPE)
  set(CANOPY_STREAMING_TLS_ENCLAVE_HAS_SGXSSL_IMPL
      FALSE
      CACHE INTERNAL "True when streaming_tls_enclave links SGXSSL implementation archives" FORCE)

  if(WIN32)
    message(STATUS "SGXSSL enclave support is not configured on Windows by this helper.")
    return()
  endif()

  option(CANOPY_BOOTSTRAP_SGXSSL
         "Prepare TLS-capable Intel SGXSSL from the local SGX submodule when enclave code needs OpenSSL" ON)

  set(_canopy_sgxssl_real_backend TRUE)
  if(CANOPY_SGX_BACKEND STREQUAL "Fake")
    set(_canopy_sgxssl_real_backend FALSE)
  endif()

  set(CANOPY_SGXSSL_ROOT_DIR
      ""
      CACHE PATH "Intel SGXSSL source directory used to prepare enclave OpenSSL support")
  if(NOT CANOPY_SGXSSL_ROOT_DIR AND _canopy_sgxssl_real_backend)
    set(CANOPY_SGXSSL_ROOT_DIR
        "${CMAKE_SOURCE_DIR}/submodules/confidential-computing.sgx/external/sgxssl"
        CACHE PATH "Intel SGXSSL source directory used to prepare enclave OpenSSL support" FORCE)
  endif()
  if(_canopy_sgxssl_real_backend
     AND CANOPY_BOOTSTRAP_SGXSSL
     AND NOT EXISTS "${CANOPY_SGXSSL_ROOT_DIR}/prepare_sgxssl.sh")
    message(
      FATAL_ERROR
        "CANOPY_BOOTSTRAP_SGXSSL is ON, but ${CANOPY_SGXSSL_ROOT_DIR}/prepare_sgxssl.sh was not found. "
        "Populate submodules/confidential-computing.sgx, or set CANOPY_SGXSSL_ROOT_DIR to an Intel SGXSSL source tree.")
  endif()

  set(CANOPY_SGXSSL_BUILD_DIR
      "${CMAKE_BINARY_DIR}/sgxssl"
      CACHE PATH "Build workspace used when preparing Intel SGXSSL from source")

  # These cache variables are user-overridable, so stale values can survive a reconfigure. SampleAttestedTLS was an
  # older source of these paths; clear it so the canonical external/sgxssl layout wins.
  foreach(_canopy_sgxssl_cache_var CANOPY_SGXSSL_INCLUDE_DIR CANOPY_SGX_OPENSSL_INCLUDE_DIR CANOPY_SGXSSL_LIB_DIR)
    if(DEFINED ${_canopy_sgxssl_cache_var} AND "${${_canopy_sgxssl_cache_var}}" MATCHES "SampleAttestedTLS")
      unset(${_canopy_sgxssl_cache_var} CACHE)
    endif()
  endforeach()

  # SGXSSL names debug archives with a trailing "d". Keep this suffix together with the DEBUG flag passed to
  # prepare_sgxssl.sh so the selected archive names match the way SGXSSL was built.
  set(_canopy_sgxssl_archive_suffix "")
  set(_canopy_sgxssl_archive_config "release")
  set(_canopy_sgxssl_debug_flag "0")
  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(_canopy_sgxssl_archive_suffix "d")
    set(_canopy_sgxssl_archive_config "debug")
    set(_canopy_sgxssl_debug_flag "1")
  endif()
  set(CANOPY_SGXSSL_ARCHIVE_SUFFIX
      "${_canopy_sgxssl_archive_suffix}"
      PARENT_SCOPE)
  set(CANOPY_SGXSSL_ARCHIVE_CONFIG
      "${_canopy_sgxssl_archive_config}"
      PARENT_SCOPE)

  # tsgxsslio.h provides SGXSSL's enclave-side OpenSSL BIO/syscall shims. With bootstrap enabled the package/include
  # directory is accepted before it exists because canopy_prepare_sgxssl will generate it during the build.
  set(CANOPY_SGXSSL_INCLUDE_DIR
      ""
      CACHE PATH "Directory containing tsgxsslio.h for enclave OpenSSL support")
  if(_canopy_sgxssl_real_backend AND CANOPY_BOOTSTRAP_SGXSSL)
    set(CANOPY_SGXSSL_INCLUDE_DIR
        "${CANOPY_SGXSSL_BUILD_DIR}/Linux/package/include"
        CACHE PATH "Directory containing tsgxsslio.h for enclave OpenSSL support" FORCE)
  endif()
  if(NOT CANOPY_SGXSSL_INCLUDE_DIR)
    set(_canopy_sgxssl_include_candidates)
    if(CANOPY_SGXSSL_ROOT_DIR AND NOT CANOPY_BOOTSTRAP_SGXSSL)
      list(APPEND _canopy_sgxssl_include_candidates "${CANOPY_SGXSSL_ROOT_DIR}/Linux/package/include")
    endif()
    list(APPEND _canopy_sgxssl_include_candidates "${CMAKE_SOURCE_DIR}/submodules/confidential-computing.sgx/sdk/ttls")

    foreach(_canopy_sgxssl_include_candidate ${_canopy_sgxssl_include_candidates})
      if(EXISTS "${_canopy_sgxssl_include_candidate}/tsgxsslio.h"
         OR (_canopy_sgxssl_real_backend
             AND CANOPY_BOOTSTRAP_SGXSSL
             AND "${_canopy_sgxssl_include_candidate}" STREQUAL "${CANOPY_SGXSSL_BUILD_DIR}/Linux/package/include"))
        set(CANOPY_SGXSSL_INCLUDE_DIR
            "${_canopy_sgxssl_include_candidate}"
            CACHE PATH "Directory containing tsgxsslio.h for enclave OpenSSL support" FORCE)
        break()
      endif()
    endforeach()
  endif()

  # The enclave cannot use the host OpenSSL headers. This must resolve to the SGX-compatible OpenSSL headers from a
  # prepared SGXSSL package or from the DCAP prebuilt fallback.
  set(CANOPY_SGX_OPENSSL_INCLUDE_DIR
      ""
      CACHE PATH "Directory containing SGX-compatible OpenSSL headers for enclave builds")
  if(_canopy_sgxssl_real_backend AND CANOPY_BOOTSTRAP_SGXSSL)
    set(CANOPY_SGX_OPENSSL_INCLUDE_DIR
        "${CANOPY_SGXSSL_BUILD_DIR}/Linux/package/include"
        CACHE PATH "Directory containing SGX-compatible OpenSSL headers for enclave builds" FORCE)
  endif()
  if(NOT CANOPY_SGX_OPENSSL_INCLUDE_DIR)
    set(_canopy_sgx_openssl_include_candidates)
    if(CANOPY_SGXSSL_INCLUDE_DIR)
      list(APPEND _canopy_sgx_openssl_include_candidates "${CANOPY_SGXSSL_INCLUDE_DIR}")
    endif()
    list(APPEND _canopy_sgx_openssl_include_candidates
         "${CMAKE_SOURCE_DIR}/submodules/confidential-computing.sgx/external/dcap_source/prebuilt/openssl/inc")

    foreach(_canopy_sgx_openssl_include_candidate ${_canopy_sgx_openssl_include_candidates})
      if(EXISTS "${_canopy_sgx_openssl_include_candidate}/openssl/ssl.h"
         OR (_canopy_sgxssl_real_backend
             AND CANOPY_BOOTSTRAP_SGXSSL
             AND "${_canopy_sgx_openssl_include_candidate}" STREQUAL "${CANOPY_SGXSSL_BUILD_DIR}/Linux/package/include"
            ))
        set(CANOPY_SGX_OPENSSL_INCLUDE_DIR
            "${_canopy_sgx_openssl_include_candidate}"
            CACHE PATH "Directory containing SGX-compatible OpenSSL headers for enclave builds" FORCE)
        break()
      endif()
    endforeach()
  endif()

  # These are Intel SGX trusted runtime libraries, not SGXSSL's OpenSSL archives. They normally come from the SGX SDK or
  # confidential-computing.sgx build output and are required for a real enclave link using SGXSSL.
  set(CANOPY_SGX_TLS_LIB_DIR
      ""
      CACHE PATH "Directory containing libsgx_ttls.a and libsgx_tcrypto.a for enclave SGXSSL builds")
  if(NOT CANOPY_SGX_TLS_LIB_DIR)
    foreach(
      _canopy_sgx_tls_lib_candidate
      "${SGX_LIBRARY_PATH}"
      "${CMAKE_SOURCE_DIR}/submodules/confidential-computing.sgx/linux/installer/common/sdk/output/package/lib64"
      "${CMAKE_SOURCE_DIR}/submodules/confidential-computing.sgx/build/linux")
      if(EXISTS "${_canopy_sgx_tls_lib_candidate}/libsgx_ttls.a"
         AND EXISTS "${_canopy_sgx_tls_lib_candidate}/libsgx_tcrypto.a"
         AND EXISTS "${_canopy_sgx_tls_lib_candidate}/libsgx_dcap_tvl.a")
        set(CANOPY_SGX_TLS_LIB_DIR
            "${_canopy_sgx_tls_lib_candidate}"
            CACHE PATH "Directory containing libsgx_ttls.a and libsgx_tcrypto.a for enclave SGXSSL builds" FORCE)
        break()
      endif()
    endforeach()
  endif()

  # These are the SGXSSL/OpenSSL implementation archives. When bootstrap is enabled they are custom-command outputs;
  # when disabled the user can point this cache variable at a prebuilt SGXSSL package.
  set(CANOPY_SGXSSL_LIB_DIR
      ""
      CACHE PATH "Directory containing libsgx_tsgxssl*.a for enclave builds")
  if(_canopy_sgxssl_real_backend AND CANOPY_BOOTSTRAP_SGXSSL)
    set(CANOPY_SGXSSL_LIB_DIR
        "${CANOPY_SGXSSL_BUILD_DIR}/Linux/package/lib64"
        CACHE PATH "Directory containing libsgx_tsgxssl*.a for enclave builds" FORCE)
  endif()
  if(NOT CANOPY_SGXSSL_LIB_DIR)
    set(_canopy_sgxssl_lib_candidates)
    if(CANOPY_SGXSSL_ROOT_DIR AND NOT CANOPY_BOOTSTRAP_SGXSSL)
      list(APPEND _canopy_sgxssl_lib_candidates "${CANOPY_SGXSSL_ROOT_DIR}/Linux/package/lib64")
    endif()

    foreach(_canopy_sgxssl_lib_candidate ${_canopy_sgxssl_lib_candidates})
      if(EXISTS "${_canopy_sgxssl_lib_candidate}/libsgx_tsgxssl${_canopy_sgxssl_archive_suffix}.a"
         AND EXISTS "${_canopy_sgxssl_lib_candidate}/libsgx_tsgxssl_ssl${_canopy_sgxssl_archive_suffix}.a"
         AND EXISTS "${_canopy_sgxssl_lib_candidate}/libsgx_tsgxssl_crypto${_canopy_sgxssl_archive_suffix}.a")
        set(CANOPY_SGXSSL_LIB_DIR
            "${_canopy_sgxssl_lib_candidate}"
            CACHE PATH "Directory containing libsgx_tsgxssl*.a for enclave builds" FORCE)
        break()
      elseif(
        _canopy_sgxssl_real_backend
        AND CANOPY_BOOTSTRAP_SGXSSL
        AND "${_canopy_sgxssl_lib_candidate}" STREQUAL "${CANOPY_SGXSSL_BUILD_DIR}/Linux/package/lib64")
        set(CANOPY_SGXSSL_LIB_DIR
            "${_canopy_sgxssl_lib_candidate}"
            CACHE PATH "Directory containing libsgx_tsgxssl*.a for enclave builds" FORCE)
        break()
      endif()
    endforeach()
  endif()

  # Header readiness and implementation readiness are deliberately separate. Fake SGX and compile-fit configurations can
  # build enclave code against headers without pretending that the final SGXSSL archives are available.
  set(_canopy_sgxssl_headers_ready FALSE)
  if(EXISTS "${CANOPY_SGXSSL_INCLUDE_DIR}/tsgxsslio.h" AND EXISTS "${CANOPY_SGX_OPENSSL_INCLUDE_DIR}/openssl/ssl.h")
    set(_canopy_sgxssl_headers_ready TRUE)
  elseif(
    _canopy_sgxssl_real_backend
    AND CANOPY_BOOTSTRAP_SGXSSL
    AND CANOPY_SGXSSL_ROOT_DIR)
    set(_canopy_sgxssl_headers_ready TRUE)
  endif()

  set(_canopy_sgxssl_lib "${CANOPY_SGXSSL_LIB_DIR}/libsgx_tsgxssl${_canopy_sgxssl_archive_suffix}.a")
  set(_canopy_sgxssl_ssl_lib "${CANOPY_SGXSSL_LIB_DIR}/libsgx_tsgxssl_ssl${_canopy_sgxssl_archive_suffix}.a")
  set(_canopy_sgxssl_crypto_lib "${CANOPY_SGXSSL_LIB_DIR}/libsgx_tsgxssl_crypto${_canopy_sgxssl_archive_suffix}.a")
  set(_canopy_sgxssl_impl_libs_found FALSE)

  if(EXISTS "${_canopy_sgxssl_lib}"
     AND EXISTS "${_canopy_sgxssl_ssl_lib}"
     AND EXISTS "${_canopy_sgxssl_crypto_lib}"
     AND NOT (_canopy_sgxssl_real_backend AND CANOPY_BOOTSTRAP_SGXSSL))
    set(_canopy_sgxssl_impl_libs_found TRUE)
  elseif(
    _canopy_sgxssl_real_backend
    AND CANOPY_BOOTSTRAP_SGXSSL
    AND CANOPY_SGXSSL_ROOT_DIR
    AND CANOPY_SGXSSL_LIB_DIR)
    find_program(CANOPY_BASH_EXECUTABLE bash)
    if(NOT CANOPY_BASH_EXECUTABLE)
      message(FATAL_ERROR "Cannot prepare SGXSSL because 'bash' was not found.")
    endif()
    find_program(CANOPY_MAKE_EXECUTABLE make)
    if(NOT CANOPY_MAKE_EXECUTABLE)
      message(FATAL_ERROR "Cannot prepare SGXSSL because 'make' was not found.")
    endif()

    set(_canopy_sgxssl_openssl_build_dir "${CANOPY_SGXSSL_BUILD_DIR}/openssl_source/openssl-3.1.6")
    set(_canopy_sgxssl_skip_intelcpu_check FALSE)
    if(NOT SGX_HW)
      set(_canopy_sgxssl_skip_intelcpu_check TRUE)
    endif()

    # Bootstrap builds SGXSSL into the binary directory without modifying the submodule. The patch step adjusts Intel's
    # script for Canopy's needs: no external pthread use from enclave OpenSSL, and no Intel CPU check when running SGX
    # simulation on non-Intel hosts.
    if(NOT TARGET canopy_prepare_sgxssl)
      add_custom_command(
        OUTPUT "${_canopy_sgxssl_lib}" "${_canopy_sgxssl_ssl_lib}" "${_canopy_sgxssl_crypto_lib}"
               "${CANOPY_SGX_OPENSSL_INCLUDE_DIR}/openssl/ssl.h"
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${CANOPY_SGXSSL_BUILD_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CANOPY_SGXSSL_BUILD_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CANOPY_SGXSSL_ROOT_DIR}/prepare_sgxssl.sh"
                "${CANOPY_SGXSSL_BUILD_DIR}/prepare_sgxssl.sh"
        COMMAND
          "${CMAKE_COMMAND}" "-DCANOPY_SGXSSL_SCRIPT=${CANOPY_SGXSSL_BUILD_DIR}/prepare_sgxssl.sh"
          "-DCANOPY_SGXSSL_SKIP_INTELCPU_CHECK=${_canopy_sgxssl_skip_intelcpu_check}" -P
          "${CMAKE_SOURCE_DIR}/cmake/CanopyPatchSGXSSLBootstrap.cmake"
        COMMAND
          "${CMAKE_COMMAND}" -E env ${CANOPY_SGX_DETERMINISTIC_ENV} "SGX_SDK=${SGX_DIR}"
          "DEBUG=${_canopy_sgxssl_debug_flag}" "HOME=${CANOPY_SGXSSL_BUILD_DIR}" "${CANOPY_BASH_EXECUTABLE}"
          "${CANOPY_SGXSSL_BUILD_DIR}/prepare_sgxssl.sh"
        # prepare_sgxssl.sh builds the package; libssl.a is then rebuilt/copied into the archive name expected by the
        # enclave link so TLS symbols are present alongside the SGXSSL glue archive.
        COMMAND "${CMAKE_COMMAND}" -E chdir "${_canopy_sgxssl_openssl_build_dir}" "${CANOPY_MAKE_EXECUTABLE}" libssl.a
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_canopy_sgxssl_openssl_build_dir}/libssl.a"
                "${_canopy_sgxssl_ssl_lib}"
        DEPENDS "${CANOPY_SGXSSL_ROOT_DIR}/prepare_sgxssl.sh"
                "${CMAKE_SOURCE_DIR}/cmake/CanopyPatchSGXSSLBootstrap.cmake"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        COMMENT "Preparing ${_canopy_sgxssl_archive_config} TLS-capable SGXSSL for enclave OpenSSL support"
        VERBATIM)
      add_custom_target(canopy_prepare_sgxssl DEPENDS "${_canopy_sgxssl_lib}" "${_canopy_sgxssl_ssl_lib}"
                                                      "${_canopy_sgxssl_crypto_lib}")
    endif()
    set(_canopy_sgxssl_impl_libs_found TRUE)
    message(STATUS "SGXSSL will be prepared from ${CANOPY_SGXSSL_ROOT_DIR} in ${CANOPY_SGXSSL_BUILD_DIR}")
  endif()

  set(_canopy_sgxssl_enclave_link_libraries)
  if(_canopy_sgxssl_impl_libs_found)
    if(NOT EXISTS "${CANOPY_SGX_TLS_LIB_DIR}/libsgx_ttls.a"
       OR NOT EXISTS "${CANOPY_SGX_TLS_LIB_DIR}/libsgx_tcrypto.a"
       OR NOT EXISTS "${CANOPY_SGX_TLS_LIB_DIR}/libsgx_dcap_tvl.a")
      message(FATAL_ERROR "SGXSSL enclave support requires libsgx_ttls.a, libsgx_tcrypto.a, and libsgx_dcap_tvl.a. "
                          "Set CANOPY_SGX_TLS_LIB_DIR to the SGX SDK lib64 directory.")
    endif()

    set(CANOPY_STREAMING_TLS_ENCLAVE_HAS_SGXSSL_IMPL
        TRUE
        CACHE INTERNAL "True when streaming_tls_enclave links SGXSSL implementation archives" FORCE)

    # libsgx_tsgxssl contains registration/glue objects that may not be referenced directly by every consumer. Mark it
    # whole-archive so FindSGX keeps those objects in the final enclave link.
    canopy_add_imported_enclave_archive(canopy_sgxssl_tsgxssl_enclave "${_canopy_sgxssl_lib}")
    set_target_properties(canopy_sgxssl_tsgxssl_enclave PROPERTIES CANOPY_SGX_WHOLE_ARCHIVE TRUE)
    canopy_add_imported_enclave_archive(canopy_sgxssl_ssl_enclave "${_canopy_sgxssl_ssl_lib}")
    canopy_add_imported_enclave_archive(canopy_sgxssl_crypto_enclave "${_canopy_sgxssl_crypto_lib}")
    canopy_add_imported_enclave_archive(canopy_sgx_ttls_enclave "${CANOPY_SGX_TLS_LIB_DIR}/libsgx_ttls.a")
    canopy_add_imported_enclave_archive(canopy_sgx_dcap_tvl_enclave "${CANOPY_SGX_TLS_LIB_DIR}/libsgx_dcap_tvl.a")

    set(_canopy_sgxssl_enclave_link_libraries
        canopy_sgxssl_tsgxssl_enclave
        canopy_sgxssl_ssl_enclave
        canopy_sgxssl_crypto_enclave
        canopy_sgx_ttls_enclave
        canopy_sgx_dcap_tvl_enclave)
  endif()

  set(CANOPY_SGXSSL_HEADERS_READY
      "${_canopy_sgxssl_headers_ready}"
      PARENT_SCOPE)
  set(CANOPY_SGXSSL_IMPL_LIBS_FOUND
      "${_canopy_sgxssl_impl_libs_found}"
      PARENT_SCOPE)
  set(CANOPY_SGXSSL_ENCLAVE_LINK_LIBRARIES
      "${_canopy_sgxssl_enclave_link_libraries}"
      PARENT_SCOPE)
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

if(${SGX_MODE} STREQUAL "release")
  set(CANOPY_SGX_CREATE_ENCLAVE_DEBUG_FLAG 0)
else()
  set(CANOPY_SGX_CREATE_ENCLAVE_DEBUG_FLAG 1)
endif()

# ######################################################################################################################
# Enclave flag for defines
# ######################################################################################################################
set(CANOPY_BUILD_ENCLAVE_FLAG CANOPY_BUILD_ENCLAVE)

# ######################################################################################################################
# Enclave format library
# ######################################################################################################################
set(CANOPY_ENCLAVE_FMT_LIB fmt::fmt-header-only)
set(CANOPY_ENCLAVE_PROTOBUF_TARGET)
set(CANOPY_SGX_PROTOC_EXECUTABLE)
set(CANOPY_ENCLAVE_PROTOBUF_DEFINES)
if(CANOPY_BUILD_NANOPB)
  list(APPEND CANOPY_ENCLAVE_PROTOBUF_DEFINES CANOPY_BUILD_NANOPB CANOPY_USE_NANOPB_FOR_PROTOCOL_BUFFERS)
endif()

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
  set(CANOPY_SHARED_ENCLAVE_DEFINES ${CANOPY_SHARED_DEFINES} FOR_SGX ${SGX_HW_OR_SIM_DEFINE}
                                    ${CANOPY_ENCLAVE_MEMLEAK_DEFINES})
  list(REMOVE_ITEM CANOPY_SHARED_ENCLAVE_DEFINES CANOPY_BUILD_PROTOCOL_BUFFERS CANOPY_USE_PROTOCOL_BUFFERS_FOR_NANOPB)
  list(APPEND CANOPY_SHARED_ENCLAVE_DEFINES ${CANOPY_ENCLAVE_PROTOBUF_DEFINES})
  list(REMOVE_DUPLICATES CANOPY_SHARED_ENCLAVE_DEFINES)

  # Enclave compile options (Windows)
  set(CANOPY_SHARED_ENCLAVE_COMPILE_OPTIONS ${CANOPY_SHARED_COMPILE_OPTIONS} /d2FH4- /Qspectre
                                             ${CANOPY_SGX_DETERMINISTIC_COMPILE_OPTIONS})

  # Enclave link options (Windows)
  set(CANOPY_SHARED_ENCLAVE_LINK_OPTIONS ${CANOPY_LINK_OPTIONS} ${CANOPY_SGX_DETERMINISTIC_LINK_OPTIONS})

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
      ${SGX_HW_OR_SIM_DEFINE}
      ${CANOPY_SHARED_DEFINES}
      CLEAN_LIBC
      ENCLAVE_STATUS=sgx_status_t
      ENCLAVE_OK=SGX_SUCCESS
      DISALLOW_BAD_JUMPS
      _LIBCPP_DISABLE_AVAILABILITY
      __THROW=)
  list(REMOVE_ITEM CANOPY_SHARED_ENCLAVE_DEFINES CANOPY_BUILD_PROTOCOL_BUFFERS CANOPY_USE_PROTOCOL_BUFFERS_FOR_NANOPB)
  list(APPEND CANOPY_SHARED_ENCLAVE_DEFINES ${CANOPY_ENCLAVE_PROTOBUF_DEFINES})
  list(REMOVE_DUPLICATES CANOPY_SHARED_ENCLAVE_DEFINES)

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
      -Wno-variadic-macros
      ${CANOPY_SGX_DETERMINISTIC_COMPILE_OPTIONS})
  set(CANOPY_ENCLAVE_LINK_OPTIONS ${CANOPY_SGX_DETERMINISTIC_LINK_OPTIONS})

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

  # Host-side SGX link options are intentionally kept separate from CANOPY_LINK_OPTIONS/CANOPY_LINK_EXE_OPTIONS so
  # non-SGX targets do not inherit SGX SDK libraries just because enclave support is enabled for the build.
  set(CANOPY_SGX_HOST_LINK_OPTIONS -L${SGX_LIBRARY_PATH})
  if(EXISTS "${SGX_LIBRARY_PATH}/libsgx_dcap_quoteverify.so")
    list(APPEND CANOPY_SGX_HOST_LINK_OPTIONS -lsgx_dcap_quoteverify)
  endif()
  if(EXISTS "${SGX_LIBRARY_PATH}/libsgx_dcap_ql.so")
    list(APPEND CANOPY_SGX_HOST_LINK_OPTIONS -lsgx_dcap_ql)
  endif()

  # Check if SGX SDK has debug information
  if(SGX_SDK_CONTAINS_DEBUG_INFORMATION)
    list(APPEND CANOPY_ENCLAVE_DEFINES SGX_SDK_CONTAINS_DEBUG_INFORMATION)
  endif()

  # Enclave include directories.
  #
  # Put the Canopy SGX polyfill directory before the SGX libc++ headers. Enclave targets compile with -nostdinc++ and
  # some third-party coroutine headers include standard names such as <coroutine>, <chrono>, and <exception> directly.
  # The leading polyfill path makes those standard-name includes resolve to the enclave-safe implementations without
  # exposing rpc/internal/polyfill/sgx/... includes from public Canopy headers.
  set(CANOPY_ENCLAVE_POLYFILL_INCLUDES ${CMAKE_SOURCE_DIR}/c++/rpc/include/rpc/internal/polyfill/sgx)
  set(CANOPY_ENCLAVE_LIBC_INCLUDES ${SGX_INCLUDE_DIR} ${SGX_TLIBC_INCLUDE_DIR})
  set(CANOPY_ENCLAVE_LIBCXX_INCLUDES ${CANOPY_ENCLAVE_POLYFILL_INCLUDES} ${CANOPY_ENCLAVE_LIBC_INCLUDES}
                                     ${SGX_LIBCXX_INCLUDE_DIR} ${SGX_LIBSTDCXX_INCLUDE_DIR})
endif()

# ######################################################################################################################
# Common SGX includes and libraries
# ######################################################################################################################
set(CANOPY_INCLUDES ${SGX_INCLUDE_DIR})

# Host-side SGX runtime libraries. Keep these separate from CANOPY_LIBRARIES so non-SGX targets in an enclave-enabled
# build do not inherit SGX link dependencies.
set(CANOPY_SGX_HOST_LIBRARIES)
if(WIN32)
  if(${SGX_HW})
    list(
      APPEND
      CANOPY_SGX_HOST_LIBRARIES
      sgx_tcrypto.lib
      sgx_uae_service.lib
      sgx_capable.lib
      sgx_urts.lib)
  else()
    list(
      APPEND
      CANOPY_SGX_HOST_LIBRARIES
      sgx_tcrypto.lib
      sgx_uae_service_sim.lib
      sgx_capable.lib
      sgx_urts_sim.lib)
  endif()
else()
  list(
    APPEND
    CANOPY_SGX_HOST_LIBRARIES
    ${SGX_USVC_LIB}
    sgx_capable
    ${SGX_URTS_LIB}
    ${SGX_TCRYPTO_LIB})
endif()

# ######################################################################################################################
# Output configuration summary
# ######################################################################################################################
message("CANOPY_ENCLAVE_DEFINES ${CANOPY_ENCLAVE_DEFINES}")
message("CANOPY_ENCLAVE_COMPILE_OPTIONS ${CANOPY_ENCLAVE_COMPILE_OPTIONS}")
message("CANOPY_ENCLAVE_LINK_OPTIONS ${CANOPY_ENCLAVE_LINK_OPTIONS}")
message(STATUS "SGX enclave support configured successfully")
