#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

# This shared object is loaded at test time by dll_transport_setup.
# It provides canopy_dll_init (yyy::i_host -> yyy::i_example via marshalled_tests::example).
# All other canopy_dll_* entry points come from transport_dynamic_library_dll.

# Only meaningful in non-coroutine builds (dynamic_library transport is non-coro only).
if(NOT CANOPY_BUILD_COROUTINE)

    add_library(canopy_example_dll SHARED src/canopy_dll_init.cpp src/rpc_log.cpp)

    target_compile_definitions(canopy_example_dll
        PRIVATE
            ${CANOPY_DEFINES}
            CANOPY_DLL_BUILDING)

    target_include_directories(
        canopy_example_dll
        PUBLIC "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated/include>"
               "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated/src>"
        PRIVATE ${CANOPY_INCLUDES})

    target_link_libraries(
        canopy_example_dll
        PRIVATE
            transport_dynamic_library_dll
            transport_local
            common_tests
            example_idl
            yas_common
            rpc::rpc
            ${CANOPY_LIBRARIES})

    target_compile_options(canopy_example_dll
        PRIVATE
            ${CANOPY_COMPILE_OPTIONS}
            ${CANOPY_WARN_OK}
            $<$<CXX_COMPILER_ID:GNU,Clang>:-fvisibility=hidden>
            $<$<CXX_COMPILER_ID:GNU,Clang>:-fvisibility-inlines-hidden>)

    target_link_options(canopy_example_dll PRIVATE ${CANOPY_LINK_EXE_OPTIONS})

    set_property(TARGET canopy_example_dll PROPERTY COMPILE_PDB_NAME canopy_example_dll)

    if(CANOPY_ENABLE_CLANG_TIDY)
        set_target_properties(canopy_example_dll PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
    endif()

endif()
