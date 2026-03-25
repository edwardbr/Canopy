#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.

   CanopyComponentInstall.cmake
   ============================
   Provides canopy_install_component() — a helper for downstream project developers
   to install their finished Canopy-based components (executables, DLLs, IDL files,
   and generated proto files) in one call.

   Typical usage (add_subdirectory pattern):
   =========================================
   In your downstream CMakeLists.txt:

       set(CANOPY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../Canopy2")
       list(APPEND CMAKE_MODULE_PATH "${CANOPY_DIR}/cmake")
       add_subdirectory("${CANOPY_DIR}")
       include(CanopyComponentInstall)

       CanopyGenerate(my_service "interface/my_service.idl" ...)

       CanopyCreateDynamicLibrary(my_plugin
           SOURCES src/my_plugin.cpp
           LINK_LIBRARIES my_service_idl)

       canopy_install_component(
           TARGETS      my_plugin my_host
           IDL_FILES    interface/my_service.idl
       )

   Install layout produced:
       bin/          - executables and shared libraries
       share/idl/    - IDL files (your public RPC API), directory structure preserved
       share/proto/  - generated .proto files discovered from the build tree
       bin/canopy_component_version.txt - Canopy protocol version stamp

   Optional arguments:
       COMPONENT   <name>   CMake install component name (passed through to install())
       DESTINATION <prefix> Override the root prefix (default: use GNUInstallDirs)
]]

cmake_minimum_required(VERSION 3.24)

# ---------------------------------------------------------------------------
# Derive Canopy protocol version from version.h at configure time. This allows the install to stamp which protocol
# version the component uses.
# ---------------------------------------------------------------------------
set(_canopy_vh "${CMAKE_CURRENT_LIST_DIR}/../rpc/include/rpc/internal/version.h")
set(_CANOPY_COMPONENT_VERSION 3) # fallback if version.h is not found

if(EXISTS "${_canopy_vh}")
  file(STRINGS "${_canopy_vh}" _ver_lines REGEX "constexpr std::uint64_t VERSION_[0-9]+ = [0-9]+")
  set(_highest 0)
  foreach(_line ${_ver_lines})
    string(REGEX MATCH "VERSION_[0-9]+ = ([0-9]+)" _m "${_line}")
    if(NOT "${CMAKE_MATCH_1}" STREQUAL "" AND CMAKE_MATCH_1 GREATER _highest)
      set(_highest "${CMAKE_MATCH_1}")
    endif()
  endforeach()
  if(_highest GREATER 0)
    set(_CANOPY_COMPONENT_VERSION "${_highest}")
  endif()
  unset(_ver_lines)
  unset(_line)
  unset(_m)
  unset(_highest)
endif()
unset(_canopy_vh)

# ---------------------------------------------------------------------------
# canopy_install_component()
# ---------------------------------------------------------------------------
function(canopy_install_component)
  set(_options)
  set(_one COMPONENT DESTINATION)
  set(_multi TARGETS IDL_FILES)
  cmake_parse_arguments(
    ARGS
    "${_options}"
    "${_one}"
    "${_multi}"
    ${ARGN})

  if(NOT ARGS_TARGETS AND NOT ARGS_IDL_FILES)
    message(FATAL_ERROR "canopy_install_component: at least one of TARGETS or IDL_FILES is required")
  endif()

  # ---- Destination roots ------------------------------------------------
  if(ARGS_DESTINATION)
    set(_bin "${ARGS_DESTINATION}/bin")
    set(_idl_dest "${ARGS_DESTINATION}/share/idl")
    set(_proto "${ARGS_DESTINATION}/share/proto")
  else()
    include(GNUInstallDirs)
    set(_bin "${CMAKE_INSTALL_BINDIR}")
    set(_idl_dest "${CMAKE_INSTALL_DATADIR}/idl")
    set(_proto "${CMAKE_INSTALL_DATADIR}/proto")
  endif()

  # ---- Optional COMPONENT passthrough -----------------------------------
  set(_comp)
  if(ARGS_COMPONENT)
    set(_comp COMPONENT "${ARGS_COMPONENT}")
  endif()

  # ---- Install targets --------------------------------------------------
  foreach(_tgt ${ARGS_TARGETS})
    if(NOT TARGET ${_tgt})
      message(WARNING "canopy_install_component: '${_tgt}' is not a CMake target — skipping")
      continue()
    endif()
    get_target_property(_type ${_tgt} TYPE)
    if(_type STREQUAL "EXECUTABLE")
      install(TARGETS ${_tgt} RUNTIME DESTINATION "${_bin}" ${_comp})
    elseif(_type STREQUAL "SHARED_LIBRARY")
      install(TARGETS ${_tgt} LIBRARY DESTINATION "${_bin}" ${_comp})
    else()
      message(WARNING "canopy_install_component: '${_tgt}' has type '${_type}' which is not handled — skipping")
    endif()
  endforeach()

  # ---- Install IDL files (public RPC API) ------------------------------
  # Directory structure relative to the caller's source dir is preserved.
  foreach(_idl_file ${ARGS_IDL_FILES})
    get_filename_component(
      _idl_abs
      "${_idl_file}"
      ABSOLUTE
      BASE_DIR
      "${CMAKE_CURRENT_SOURCE_DIR}")
    file(RELATIVE_PATH _idl_rel "${CMAKE_CURRENT_SOURCE_DIR}" "${_idl_abs}")
    get_filename_component(_idl_dir "${_idl_rel}" DIRECTORY)
    if(_idl_dir)
      install(
        FILES "${_idl_abs}"
        DESTINATION "${_idl_dest}/${_idl_dir}"
        ${_comp})
    else()
      install(
        FILES "${_idl_abs}"
        DESTINATION "${_idl_dest}"
        ${_comp})
    endif()
  endforeach()

  # ---- Auto-discover generated .proto files ----------------------------
  # CanopyGenerate() places proto files under: <CMAKE_BINARY_DIR>/generated/src/<sub_directory>/protobuf/*.proto The
  # relative path under generated/src/ is preserved in the install.
  set(_proto_root "${CMAKE_BINARY_DIR}/generated/src")
  if(EXISTS "${_proto_root}")
    file(GLOB_RECURSE _protos "${_proto_root}/*.proto")
    foreach(_pf ${_protos})
      file(RELATIVE_PATH _pf_rel "${_proto_root}" "${_pf}")
      get_filename_component(_pf_dir "${_pf_rel}" DIRECTORY)
      if(_pf_dir)
        install(
          FILES "${_pf}"
          DESTINATION "${_proto}/${_pf_dir}"
          ${_comp})
      else()
        install(
          FILES "${_pf}"
          DESTINATION "${_proto}"
          ${_comp})
      endif()
    endforeach()
  endif()

  # ---- Version stamp ----------------------------------------------------
  # Recipients can read this file to check protocol compatibility.
  set(_stamp_file "${CMAKE_CURRENT_BINARY_DIR}/canopy_component_version.txt")
  file(WRITE "${_stamp_file}" "canopy_version=${_CANOPY_COMPONENT_VERSION}\n")
  install(
    FILES "${_stamp_file}"
    DESTINATION "${_bin}"
    ${_comp})

endfunction()
