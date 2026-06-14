#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

# Test DLL for the shared_scheduler_dll transport (coroutine builds only). Provides
# canopy_module_init wired to marshalled_tests::example.

if(NOT CANOPY_BUILD_COROUTINE)
  return()
endif()

canopycreatesharedschedulerdll(
  canopy_shared_scheduler_example_dll
  SOURCES
  src/shared_scheduler_dll_init.cpp
  LINK_LIBRARIES
  transport_local
  common_tests
  example_idl
  rpc_log)
