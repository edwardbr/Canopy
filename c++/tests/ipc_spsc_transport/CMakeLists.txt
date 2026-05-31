#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

if(NOT CANOPY_BUILD_COROUTINE)
  return()
endif()

include(CanopyCreateIpcSpscDynamicLibrary)

canopycreateipcspscdynamiclibrary(
  canopy_ipc_spsc_example_dll
  SOURCES
  src/canopy_ipc_spsc_dll_init.cpp
  LINK_LIBRARIES
  transport_local
  common_tests
  example_idl
  rpc_log)

add_executable(canopy_ipc_spsc_peer_process src/ipc_spsc_peer_process_main.cpp)
target_compile_definitions(canopy_ipc_spsc_peer_process PRIVATE ${CANOPY_DEFINES})
target_include_directories(canopy_ipc_spsc_peer_process PRIVATE ${CANOPY_INCLUDES})
target_link_libraries(
  canopy_ipc_spsc_peer_process
  PRIVATE transport_ipc_spsc
          transport_streaming
          streaming_spsc_queue
          common_tests
          test_fixtures
          example_idl
          rpc::rpc
          ${CANOPY_LIBRARIES}
          rpc_log)
target_compile_options(canopy_ipc_spsc_peer_process PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(canopy_ipc_spsc_peer_process PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
set_property(TARGET canopy_ipc_spsc_peer_process PROPERTY COMPILE_PDB_NAME canopy_ipc_spsc_peer_process)
