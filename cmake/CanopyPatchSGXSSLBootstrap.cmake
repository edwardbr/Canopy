#
# Copyright (c) 2026 Edward Boggis-Rolfe All rights reserved.
#

# This script patches a build-local copy of Intel's prepare_sgxssl.sh. It must never be run against the source submodule
# directly.
#
# The copied script needs three Canopy-specific changes: 1. Build against the SGX SDK selected by the active CMake
# preset instead of assuming it is running inside a full linux-sgx source build. 2. Force OpenSSL's no-threads option so
# enclave OpenSSL does not pull in pthread symbols or require sgx_pthread.edl imports. 3. In SGX simulation only, skip
# SGXSSL's CPU-vendor guard. Hardware SGX is Intel-only, but simulation is useful on non-Intel developer hosts; this
# does not disable TLS verification or any real-SGX runtime check.

if(NOT DEFINED CANOPY_SGXSSL_SCRIPT)
  message(FATAL_ERROR "CANOPY_SGXSSL_SCRIPT must point at the copied SGXSSL prepare script.")
endif()

file(READ "${CANOPY_SGXSSL_SCRIPT}" _canopy_sgxssl_script)

# The SGXSSL script is stored inside the Intel linux-sgx tree, so its default build mode assumes the full linux-sgx
# source layout is available above it. Canopy prepares SGXSSL in a clean build-local workspace and points SGX_SDK at the
# SDK built by the active preset, so the copied script must use the SDK layout instead.
string(FIND "${_canopy_sgxssl_script}" "LINUX_SGX_BUILD=1" _canopy_sgxssl_linux_sgx_build_pos)
if(_canopy_sgxssl_linux_sgx_build_pos EQUAL -1)
  message(FATAL_ERROR "Cannot patch ${CANOPY_SGXSSL_SCRIPT}: expected LINUX_SGX_BUILD=1 was not found.")
endif()
string(REPLACE "LINUX_SGX_BUILD=1" "LINUX_SGX_BUILD=0" _canopy_sgxssl_script "${_canopy_sgxssl_script}")

# Canopy's enclave TLS stream does not need OpenSSL's pthread integration. If SGXSSL builds OpenSSL with pthread
# support, the enclave either has to link libsgx_pthread.a and import sgx_pthread.edl or fail on pthread symbols. Keep
# the enclave surface smaller by forcing the copied OpenSSL build script to add OpenSSL's no-threads option. The OpenSSL
# build script may be downloaded by prepare_sgxssl.sh, so this check/sed pair is injected immediately before
# prepare_sgxssl.sh enters the Linux directory and invokes make.
string(FIND "${_canopy_sgxssl_script}" "pushd $top_dir/Linux/" _canopy_sgxssl_linux_pushd_pos)
if(_canopy_sgxssl_linux_pushd_pos EQUAL -1)
  message(FATAL_ERROR "Cannot patch ${CANOPY_SGXSSL_SCRIPT}: expected pushd $top_dir/Linux/ was not found.")
endif()
string(
  REPLACE
    "pushd $top_dir/Linux/"
    "grep -q -- 'no-afalgeng' \"$build_script\" || { echo \"Canopy SGXSSL patch expected no-afalgeng in $build_script\"; exit 1; }\nsed -i -- 's/no-afalgeng/no-afalgeng no-threads/g' \"$build_script\" || exit 1\npushd $top_dir/Linux/"
    _canopy_sgxssl_script
    "${_canopy_sgxssl_script}")

if(CANOPY_SGXSSL_SKIP_INTELCPU_CHECK)
  # SGX simulation can run on non-Intel development hosts. SGXSSL's trusted library has an Intel CPU-vendor guard that
  # is valid for hardware SGX but too strict for simulation. Hardware SGX builds keep the guard enabled.
  string(FIND "${_canopy_sgxssl_script}" "DEBUG=$DEBUG" _canopy_sgxssl_debug_pos)
  if(_canopy_sgxssl_debug_pos EQUAL -1)
    message(FATAL_ERROR "Cannot patch ${CANOPY_SGXSSL_SCRIPT}: expected DEBUG=$DEBUG was not found.")
  endif()
  string(REPLACE "DEBUG=$DEBUG" "DEBUG=$DEBUG SKIP_INTELCPU_CHECK=TRUE" _canopy_sgxssl_script
                 "${_canopy_sgxssl_script}")
endif()

file(WRITE "${CANOPY_SGXSSL_SCRIPT}" "${_canopy_sgxssl_script}")
