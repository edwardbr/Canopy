#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

if(NOT CANOPY_BUILD_COROUTINE)
  add_library(canopy_example_c_abi_dll SHARED src/canopy_dll_init.cpp)

  target_compile_definitions(canopy_example_c_abi_dll PRIVATE ${CANOPY_DEFINES} CANOPY_C_ABI_DLL_BUILDING)

  target_include_directories(
    canopy_example_c_abi_dll
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated/include>"
           "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated/src>"
    PRIVATE ${CANOPY_INCLUDES})

  target_link_libraries(
    canopy_example_c_abi_dll
    PRIVATE -Wl,--whole-archive
            transport_c_abi_dll
            -Wl,--no-whole-archive
            transport_local
            common_tests
            example_idl
            yas_common
            rpc::rpc
            rpc_log
            ${CANOPY_LIBRARIES})

  target_compile_options(
    canopy_example_c_abi_dll
    PRIVATE ${CANOPY_COMPILE_OPTIONS}
            ${CANOPY_WARN_OK}
            $<$<CXX_COMPILER_ID:GNU,Clang>:-fvisibility=hidden>
            $<$<CXX_COMPILER_ID:GNU,Clang>:-fvisibility-inlines-hidden>)

  target_link_options(canopy_example_c_abi_dll PRIVATE ${CANOPY_LINK_EXE_OPTIONS})

  set_property(TARGET canopy_example_c_abi_dll PROPERTY COMPILE_PDB_NAME canopy_example_c_abi_dll)

  if(CANOPY_ENABLE_CLANG_TIDY)
    set_target_properties(canopy_example_c_abi_dll PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
  endif()
endif()
