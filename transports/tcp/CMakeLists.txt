#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

# TCP transport is host-only and requires coroutines
if(CANOPY_BUILD_COROUTINE)

  # Add interface subdirectory for IDL generation
  add_subdirectory(interface)

  # the transport library
  add_library(transport_tcp src/transport.cpp)

  target_compile_definitions(transport_tcp PRIVATE ${CANOPY_DEFINES})

  target_include_directories(
    transport_tcp
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    PRIVATE ${CANOPY_INCLUDES})

  target_link_libraries(
    transport_tcp
    PUBLIC streaming transport_streaming
    PRIVATE yas_common rpc_types_idl libcoro ${CANOPY_LIBRARIES})

  target_compile_options(transport_tcp PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
  target_link_options(transport_tcp PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
  set_property(TARGET transport_tcp PROPERTY COMPILE_PDB_NAME transport_tcp)

  if(CANOPY_ENABLE_CLANG_TIDY)
    set_target_properties(transport_tcp PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
  endif()

  add_library(transport_tcp_test INTERFACE)
  target_include_directories(transport_tcp_test INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/tests>")
endif()
