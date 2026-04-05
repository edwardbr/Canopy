#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

add_library(rpc_log STATIC src/rpc_log.cpp)

target_compile_definitions(rpc_log PRIVATE ${CANOPY_DEFINES})
target_compile_options(rpc_log PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(rpc_log PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
target_link_options(rpc_log INTERFACE "LINKER:--undefined=rpc_log")

set_property(TARGET rpc_log PROPERTY COMPILE_PDB_NAME rpc_log)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(rpc_log PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()

message(STATUS "rpc_log configured")
