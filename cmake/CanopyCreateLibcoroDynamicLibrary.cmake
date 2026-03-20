#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

function(CanopyCreateLibcoroDynamicLibrary target_name)
    set(options)
    set(oneValueArgs)
    set(multiValueArgs SOURCES LINK_LIBRARIES COMPILE_DEFINITIONS)
    cmake_parse_arguments(CANOPY_PLUGIN "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT CANOPY_PLUGIN_SOURCES)
        message(FATAL_ERROR "CanopyCreateLibcoroDynamicLibrary(${target_name}) requires SOURCES")
    endif()

    add_library(${target_name} SHARED ${CANOPY_PLUGIN_SOURCES})

    target_compile_definitions(${target_name}
        PRIVATE
            ${CANOPY_DEFINES}
            CANOPY_LIBCORO_DLL_BUILDING
            ${CANOPY_PLUGIN_COMPILE_DEFINITIONS})

    target_include_directories(
        ${target_name}
        PUBLIC "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated/include>"
               "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated/src>"
        PRIVATE ${CANOPY_INCLUDES})

    target_link_libraries(
        ${target_name}
        PRIVATE
            -Wl,--whole-archive
            transport_libcoro_dynamic_library_dll
            -Wl,--no-whole-archive
            ${CANOPY_PLUGIN_LINK_LIBRARIES}
            yas_common
            rpc::rpc
            ${CANOPY_LIBRARIES})

    target_compile_options(${target_name}
        PRIVATE
            ${CANOPY_COMPILE_OPTIONS}
            ${CANOPY_WARN_OK}
            $<$<CXX_COMPILER_ID:GNU,Clang>:-fvisibility=hidden>
            $<$<CXX_COMPILER_ID:GNU,Clang>:-fvisibility-inlines-hidden>)

    target_link_options(${target_name} PRIVATE ${CANOPY_LINK_EXE_OPTIONS})

    set_property(TARGET ${target_name} PROPERTY COMPILE_PDB_NAME ${target_name})

    if(CANOPY_ENABLE_CLANG_TIDY)
        set_target_properties(${target_name} PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
    endif()
endfunction()
