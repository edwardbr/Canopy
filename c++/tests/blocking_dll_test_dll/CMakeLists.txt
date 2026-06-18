#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

# This shared object is loaded at test time by blocking_dll_transport_setup. It provides canopy_module_init (yyy::i_host
# -> yyy::i_example via marshalled_tests::example). The object registration adapter supplies canopy_dll_init; all other
# canopy_dll_* entry points come from transport_blocking_dll_runtime.

# Only meaningful in non-coroutine builds (blocking_dll transport is non-coro only).
if(NOT CANOPY_BUILD_COROUTINE)
  canopycreateblockingdll(
    canopy_example_blocking_dll
    SOURCES
    src/dll_init.cpp
    LINK_LIBRARIES
    transport_local
    common_tests
    example_idl
    rpc_log)

endif()
