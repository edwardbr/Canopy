#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

# Test DLL for the libcoro_dynamic_library transport (coroutine builds only).
# Provides canopy_libcoro_dll_init wired to marshalled_tests::example.

if(NOT CANOPY_BUILD_COROUTINE)
    return()
endif()

add_library(canopy_libcoro_example_dll SHARED
    src/canopy_libcoro_dll_init.cpp
    src/rpc_log.cpp)

target_compile_definitions(canopy_libcoro_example_dll
    PRIVATE
        ${CANOPY_DEFINES}
        CANOPY_LIBCORO_DLL_BUILDING)

target_include_directories(
    canopy_libcoro_example_dll
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated/include>"
           "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated/src>"
    PRIVATE ${CANOPY_INCLUDES})

target_link_libraries(
    canopy_libcoro_example_dll
    PRIVATE
        transport_libcoro_dynamic_library_dll
        transport_local
        common_tests
        example_idl
        yas_common
        rpc::rpc
        ${CANOPY_LIBRARIES})

target_compile_options(canopy_libcoro_example_dll
    PRIVATE
        ${CANOPY_COMPILE_OPTIONS}
        ${CANOPY_WARN_OK}
        $<$<CXX_COMPILER_ID:GNU,Clang>:-fvisibility=hidden>
        $<$<CXX_COMPILER_ID:GNU,Clang>:-fvisibility-inlines-hidden>)

target_link_options(canopy_libcoro_example_dll PRIVATE ${CANOPY_LINK_EXE_OPTIONS})

set_property(
    TARGET canopy_libcoro_example_dll
    PROPERTY COMPILE_PDB_NAME canopy_libcoro_example_dll)

if(CANOPY_ENABLE_CLANG_TIDY)
    set_target_properties(
        canopy_libcoro_example_dll
        PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()
