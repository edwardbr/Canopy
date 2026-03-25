#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

# This shared object is loaded at test time by dll_transport_setup. It provides canopy_dll_init (yyy::i_host ->
# yyy::i_example via marshalled_tests::example). All other canopy_dll_* entry points come from
# transport_dynamic_library_dll.

# Only meaningful in non-coroutine builds (dynamic_library transport is non-coro only).
if(NOT CANOPY_BUILD_COROUTINE)
  canopycreatedynamiclibrary(
    canopy_example_dll
    SOURCES
    src/canopy_dll_init.cpp
    LINK_LIBRARIES
    transport_local
    common_tests
    example_idl
    rpc_log)

endif()
