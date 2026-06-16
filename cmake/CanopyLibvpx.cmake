#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.

   Demo-scoped libvpx integration. libvpx ships its own configure+make build
   (not CMake), so this drives it via a custom command and exposes the
   resulting static archive as an imported target `canopy_libvpx`.

   Only VP8 enc+dec is built (the video demo's browser uses VP8). Requires
   nasm >= 2.14 on PATH for the x86 assembly paths — see the repository
   README prerequisites.

]]

if(TARGET canopy_libvpx)
  return()
endif()

set(CANOPY_LIBVPX_SRC_DIR "${CMAKE_SOURCE_DIR}/c++/submodules/libvpx")
set(CANOPY_LIBVPX_BUILD_DIR "${CMAKE_BINARY_DIR}/libvpx-build")
set(CANOPY_LIBVPX_ARCHIVE "${CANOPY_LIBVPX_BUILD_DIR}/libvpx.a")
set(CANOPY_LIBVPX_BUILD_JOBS
    ""
    CACHE STRING "Parallel job count for libvpx make invocations; empty uses the detected CPU count")

set(_canopy_libvpx_build_jobs "${CANOPY_LIBVPX_BUILD_JOBS}")
if(NOT _canopy_libvpx_build_jobs)
  include(ProcessorCount)
  ProcessorCount(_canopy_libvpx_build_jobs)
  if(_canopy_libvpx_build_jobs EQUAL 0)
    set(_canopy_libvpx_build_jobs 1)
  endif()
endif()

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

# Configure + build in one custom command. Re-runs only if the archive is missing; libvpx's own make handles incremental
# rebuilds if its dir is kept.
add_custom_command(
  OUTPUT "${CANOPY_LIBVPX_ARCHIVE}"
  WORKING_DIRECTORY "${CANOPY_LIBVPX_BUILD_DIR}"
  COMMAND
    "${CANOPY_LIBVPX_SRC_DIR}/configure" --target=x86_64-linux-gcc --disable-vp9 --enable-vp8 --enable-vp8-encoder
    --enable-vp8-decoder --disable-examples --disable-tools --disable-docs --disable-unit-tests --disable-shared
    --enable-static --enable-pic --as=nasm
  COMMAND ${CMAKE_COMMAND} -E env make -j${_canopy_libvpx_build_jobs}
  COMMENT "Configuring and building libvpx (VP8 enc+dec, static)"
  VERBATIM)

add_custom_target(canopy_libvpx_build DEPENDS "${CANOPY_LIBVPX_ARCHIVE}")

add_library(canopy_libvpx STATIC IMPORTED GLOBAL)
set_target_properties(
  canopy_libvpx PROPERTIES IMPORTED_LOCATION "${CANOPY_LIBVPX_ARCHIVE}"
                           INTERFACE_INCLUDE_DIRECTORIES "${CANOPY_LIBVPX_SRC_DIR};${CANOPY_LIBVPX_BUILD_DIR}")
add_dependencies(canopy_libvpx canopy_libvpx_build)
