#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

add_library(canopy_network_config STATIC src/network_args.cpp src/network_auto_detect.cpp)

target_include_directories(
  canopy_network_config
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  PRIVATE ${CANOPY_INCLUDES})

target_compile_definitions(canopy_network_config PRIVATE ${CANOPY_DEFINES})
target_compile_options(canopy_network_config PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(canopy_network_config PRIVATE ${CANOPY_LINK_EXE_OPTIONS})

# args is an INTERFACE (header-only) target from submodules/args rpc transitively provides rpc_types_idl (for
# zone_address, zone, object) CANOPY_FMT_LIB is fmt::fmt-header-only, set in cmake/Canopy.cmake
target_link_libraries(
  canopy_network_config
  PUBLIC args rpc_types_idl
  PRIVATE ${CANOPY_FMT_LIB})

set_property(TARGET canopy_network_config PROPERTY COMPILE_PDB_NAME canopy_network_config)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(canopy_network_config PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()

message(STATUS "canopy_network_config configured")
