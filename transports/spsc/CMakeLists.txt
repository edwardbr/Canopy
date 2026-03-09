#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

# SPSC transport is host-only and requires coroutines
if(CANOPY_BUILD_COROUTINE)

  # Add interface subdirectory for IDL generation
  add_subdirectory(interface)

  # the transport library
  add_library(transport_spsc src/transport.cpp)

  target_compile_definitions(transport_spsc PRIVATE ${CANOPY_DEFINES})

  target_include_directories(
    transport_spsc
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    PRIVATE ${CANOPY_INCLUDES})

  target_link_libraries(
    transport_spsc
    PUBLIC streaming
    PRIVATE yas_common rpc_types_idl libcoro ${CANOPY_LIBRARIES})

  target_compile_options(transport_spsc PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
  target_link_options(transport_spsc PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
  set_property(TARGET transport_spsc PROPERTY COMPILE_PDB_NAME transport_spsc)

  if(CANOPY_ENABLE_CLANG_TIDY)
    set_target_properties(transport_spsc PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
  endif()

  add_library(transport_spsc_test INTERFACE)
  target_include_directories(transport_spsc_test INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/tests>")
endif()
