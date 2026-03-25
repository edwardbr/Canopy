#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

# Streaming transport is host-only and requires coroutines
if(CANOPY_BUILD_COROUTINE)

  # Add interface subdirectory for IDL generation
  add_subdirectory(interface)

  # the transport library
  add_library(transport_streaming src/transport.cpp)

  target_compile_definitions(transport_streaming PRIVATE ${CANOPY_DEFINES})

  target_include_directories(
    transport_streaming
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include>"
    PRIVATE ${CANOPY_INCLUDES})

  target_link_libraries(
    transport_streaming
    PUBLIC streaming
    PRIVATE yas_common
            streaming_transport_idl
            rpc_types_idl
            libcoro
            ${CANOPY_LIBRARIES})

  target_compile_options(transport_streaming PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
  target_link_options(transport_streaming PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
  set_property(TARGET transport_streaming PROPERTY COMPILE_PDB_NAME transport_streaming)

  if(CANOPY_ENABLE_CLANG_TIDY)
    set_target_properties(transport_streaming PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
  endif()

  add_library(transport_streaming_test INTERFACE)
  target_include_directories(transport_streaming_test INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/tests>")
  target_link_libraries(transport_streaming_test INTERFACE streaming streaming_tcp streaming_spsc_queue
                                                           $<$<STREQUAL:${CMAKE_SYSTEM_NAME},Linux>:streaming_io_uring>)

endif()
