#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

CanopyGenerate(
  network_config
  network_config/network_config.idl
  ${CMAKE_CURRENT_SOURCE_DIR}/interfaces
  ${CMAKE_BINARY_DIR}/generated
  ""
  yas_binary
  yas_compressed_binary
  yas_json
  protocol_buffers
  nanopb
  dependencies rpc_types_idl
  include_paths ${CMAKE_CURRENT_SOURCE_DIR}/interfaces ${CANOPY_INTERFACE_SOURCE_DIR}
  install_dir ${GENERATED_INSTALL_DIR})

target_include_directories(network_config_idl INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
                                                        "$<INSTALL_INTERFACE:include>")

add_library(network_config STATIC src/network_args.cpp src/network_auto_detect.cpp)
add_library(canopy_network_config ALIAS network_config)

target_include_directories(
  network_config
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include>"
  PRIVATE ${CANOPY_INCLUDES})

target_compile_definitions(network_config PRIVATE ${CANOPY_DEFINES})
target_compile_options(network_config PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_PEDANTIC})
target_link_options(network_config PRIVATE ${CANOPY_LINK_EXE_OPTIONS})

# args is an INTERFACE (header-only) target from submodules/args rpc transitively provides rpc_types_idl (for
# zone_address, zone, object) CANOPY_FMT_LIB is fmt::fmt-header-only, set in cmake/Canopy.cmake
target_link_libraries(
  network_config
  PUBLIC args network_config_idl rpc_types_idl
  PRIVATE ${CANOPY_FMT_LIB})

set_property(TARGET network_config PROPERTY COMPILE_PDB_NAME network_config)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(network_config PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()

message(STATUS "network_config configured")
