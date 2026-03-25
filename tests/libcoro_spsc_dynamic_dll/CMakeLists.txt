#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

if(NOT CANOPY_BUILD_COROUTINE)
  return()
endif()

canopycreatelibcorospscdynamiclibrary(
  canopy_libcoro_spsc_example_dll
  SOURCES
  src/canopy_libcoro_spsc_dll_init.cpp
  LINK_LIBRARIES
  transport_local
  common_tests
  example_idl
  rpc_log)
