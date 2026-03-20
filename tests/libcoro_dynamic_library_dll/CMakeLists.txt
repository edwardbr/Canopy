#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

# Test DLL for the libcoro_dynamic_library transport (coroutine builds only).
# Provides canopy_libcoro_dll_init wired to marshalled_tests::example.

if(NOT CANOPY_BUILD_COROUTINE)
    return()
endif()

CanopyCreateLibcoroDynamicLibrary(
    canopy_libcoro_example_dll
    SOURCES
        src/canopy_libcoro_dll_init.cpp
        src/rpc_log.cpp
    LINK_LIBRARIES
        transport_local
        common_tests
        example_idl)
