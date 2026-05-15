#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.

   Demo-scoped libvpx integration. libvpx ships its own configure+make build
   (not CMake), so this drives it via a custom command and exposes the
   resulting static archive as an imported target `canopy_libvpx`.

   Only VP8 enc+dec is built (the video demo's browser uses VP8). Requires
   nasm >= 2.14 on PATH for the x86 assembly paths — see the repository
   README prerequisites.

   SGX NOTE (phase 4, not implemented yet): an in-enclave libvpx will need a
   reproducible/deterministic build so the enclave measurement (MRENCLAVE) is
   stable. libvpx's configure+make is not reproducible as-is (build paths,
   timestamps, parallel link ordering). When the codec moves into the enclave
   this custom-command approach must be replaced with a pinned, deterministic
   build. Deliberately deferred.
]]

if(TARGET canopy_libvpx)
  return()
endif()

set(CANOPY_LIBVPX_SRC_DIR "${CMAKE_SOURCE_DIR}/c++/submodules/libvpx")
set(CANOPY_LIBVPX_BUILD_DIR "${CMAKE_BINARY_DIR}/libvpx-build")
set(CANOPY_LIBVPX_ARCHIVE "${CANOPY_LIBVPX_BUILD_DIR}/libvpx.a")

if(NOT EXISTS "${CANOPY_LIBVPX_SRC_DIR}/configure")
  message(FATAL_ERROR "libvpx submodule not present at ${CANOPY_LIBVPX_SRC_DIR}. "
                      "Clone it (git submodule update --init) before configuring the video demo.")
endif()

find_program(CANOPY_NASM_EXECUTABLE nasm)
if(NOT CANOPY_NASM_EXECUTABLE)
  message(FATAL_ERROR "nasm not found. libvpx requires nasm >= 2.14 for the video demo. "
                      "Install it (Fedora: sudo dnf install nasm) and reconfigure.")
endif()

file(MAKE_DIRECTORY "${CANOPY_LIBVPX_BUILD_DIR}")

# Configure + build in one custom command. Re-runs only if the archive is
# missing; libvpx's own make handles incremental rebuilds if its dir is kept.
add_custom_command(
  OUTPUT "${CANOPY_LIBVPX_ARCHIVE}"
  WORKING_DIRECTORY "${CANOPY_LIBVPX_BUILD_DIR}"
  COMMAND
    "${CANOPY_LIBVPX_SRC_DIR}/configure" --target=x86_64-linux-gcc --disable-vp9 --enable-vp8
    --enable-vp8-encoder --enable-vp8-decoder --disable-examples --disable-tools --disable-docs
    --disable-unit-tests --disable-shared --enable-static --enable-pic --as=nasm
  COMMAND ${CMAKE_COMMAND} -E env make -j4
  COMMENT "Configuring and building libvpx (VP8 enc+dec, static)"
  VERBATIM)

add_custom_target(canopy_libvpx_build DEPENDS "${CANOPY_LIBVPX_ARCHIVE}")

add_library(canopy_libvpx STATIC IMPORTED GLOBAL)
set_target_properties(
  canopy_libvpx
  PROPERTIES IMPORTED_LOCATION "${CANOPY_LIBVPX_ARCHIVE}"
             INTERFACE_INCLUDE_DIRECTORIES "${CANOPY_LIBVPX_SRC_DIR};${CANOPY_LIBVPX_BUILD_DIR}")
add_dependencies(canopy_libvpx canopy_libvpx_build)

# ---------------------------------------------------------------------------
# Enclave variant: canopy_libvpx_enclave
# ---------------------------------------------------------------------------
# Built against the SGX freestanding toolchain (tlibc + the project's enclave
# polyfill, -nostdinc). For replicable builds the target CPU feature set is an
# explicit, recorded build input — NOT runtime-probed: a replicator pins
# CANOPY_LIBVPX_ENCLAVE_CPU to their target machine so the enclave bytes (and
# MRENCLAVE) are deterministic.
#
#   generic  -> --target=generic-gnu : pure C, no x86 asm, no nasm, no
#               runtime CPUID. Maximally portable + deterministic. Default.
#   sse2 / sse4_1 / avx2 / ...
#            -> --target=x86_64-linux-gcc --disable-runtime-cpu-detect plus
#               -m<feature> in extra-cflags; pinned ISA, still no probe.
if(CANOPY_BUILD_ENCLAVE)
  set(CANOPY_LIBVPX_ENCLAVE_CPU
      "generic"
      CACHE STRING "libvpx enclave target CPU feature set (replicable builds pin this to the target machine)")
  set_property(CACHE CANOPY_LIBVPX_ENCLAVE_CPU PROPERTY STRINGS generic sse2 sse4_1 avx avx2)

  set(CANOPY_LIBVPX_ENCLAVE_BUILD_DIR "${CMAKE_BINARY_DIR}/libvpx-build-enclave")
  set(CANOPY_LIBVPX_ENCLAVE_ARCHIVE "${CANOPY_LIBVPX_ENCLAVE_BUILD_DIR}/libvpx.a")
  file(MAKE_DIRECTORY "${CANOPY_LIBVPX_ENCLAVE_BUILD_DIR}")

  if(CANOPY_LIBVPX_ENCLAVE_CPU STREQUAL "generic")
    set(_canopy_libvpx_enc_target "generic-gnu")
    set(_canopy_libvpx_enc_isa_cflags "")
  else()
    set(_canopy_libvpx_enc_target "x86_64-linux-gcc")
    set(_canopy_libvpx_enc_isa_cflags "-m${CANOPY_LIBVPX_ENCLAVE_CPU}")
  endif()

  # Enclave freestanding cflags: polyfill first (shadows tlibc gaps), then the
  # SGX libc includes, -nostdinc, PIC. Derived from the toolchain include vars
  # Canopy computed — no hardcoded SGX paths. Deliberately NOT forwarding
  # CANOPY_ENCLAVE_DEFINES: libvpx is a self-contained C library that needs
  # only the freestanding toolchain, not Canopy app defines — and libvpx bakes
  # the configure command line verbatim into vpx_config.c as a C string, so
  # path/quoted defines (RUNTIME_DIR="/var/secretarium/runtime/", TEMP_DIR…)
  # would break that generated literal.
  set(_canopy_libvpx_enc_cflags "-nostdinc -fPIC ${_canopy_libvpx_enc_isa_cflags}")
  foreach(_inc IN LISTS CANOPY_ENCLAVE_POLYFILL_INCLUDES CANOPY_ENCLAVE_LIBC_INCLUDES)
    string(APPEND _canopy_libvpx_enc_cflags " -I${_inc}")
  endforeach()

  add_custom_command(
    OUTPUT "${CANOPY_LIBVPX_ENCLAVE_ARCHIVE}"
    WORKING_DIRECTORY "${CANOPY_LIBVPX_ENCLAVE_BUILD_DIR}"
    COMMAND
      ${CMAKE_COMMAND} -E env CC=clang "${CANOPY_LIBVPX_SRC_DIR}/configure"
      --target=${_canopy_libvpx_enc_target} --disable-vp9 --enable-vp8 --enable-vp8-encoder
      --enable-vp8-decoder --disable-examples --disable-tools --disable-docs --disable-unit-tests
      --disable-multithread --disable-runtime-cpu-detect --disable-shared --enable-static --enable-pic
      "--extra-cflags=${_canopy_libvpx_enc_cflags}"
    # Build only the C static archive — skip the optional C++ rate-control
    # lib (libvpxrc / vp8_ratectrl_rtc.cc), which needs the C++ stdlib and is
    # not consumed here (we use the C vpx_codec_* API only).
    COMMAND ${CMAKE_COMMAND} -E env make -j4 libvpx.a
    COMMENT "Configuring and building libvpx for enclave (VP8, ${CANOPY_LIBVPX_ENCLAVE_CPU}, freestanding)"
    VERBATIM)

  add_custom_target(canopy_libvpx_enclave_build DEPENDS "${CANOPY_LIBVPX_ENCLAVE_ARCHIVE}")

  add_library(canopy_libvpx_enclave STATIC IMPORTED GLOBAL)
  set_target_properties(
    canopy_libvpx_enclave
    PROPERTIES IMPORTED_LOCATION "${CANOPY_LIBVPX_ENCLAVE_ARCHIVE}"
               INTERFACE_INCLUDE_DIRECTORIES "${CANOPY_LIBVPX_SRC_DIR};${CANOPY_LIBVPX_ENCLAVE_BUILD_DIR}")
  add_dependencies(canopy_libvpx_enclave canopy_libvpx_enclave_build)
endif()
