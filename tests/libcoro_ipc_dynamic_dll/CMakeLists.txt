#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

if(NOT CANOPY_BUILD_COROUTINE)
  return()
endif()

canopycreatelibcoroipcdynamiclibrary(
  canopy_libcoro_ipc_example_dll
  SOURCES
  src/canopy_libcoro_ipc_dll_init.cpp
  src/rpc_log.cpp
  LINK_LIBRARIES
  transport_local
  common_tests
  example_idl)
