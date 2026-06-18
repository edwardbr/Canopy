#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)
project(
  demo_text_tools_rpc_object
  VERSION 1.0.0
  LANGUAGES CXX)

include(CanopyCreateBlockingDll)
include(CanopyCreateSharedSchedulerDll)

CanopyGenerate(
  text_tools
  text_tools/text_tools.idl
  ${CMAKE_CURRENT_SOURCE_DIR}/idl
  ${CMAKE_BINARY_DIR}/generated
  ""
  yas_binary
  yas_json
  protocol_buffers
  dependencies rpc_types_idl
  include_paths ${CMAKE_CURRENT_SOURCE_DIR}/idl ${CANOPY_INTERFACE_SOURCE_DIR}
  install_dir ${GENERATED_INSTALL_DIR})

add_library(demo_text_tools_rpc_object STATIC src/text_tools.cpp)
target_compile_definitions(demo_text_tools_rpc_object PRIVATE ${CANOPY_DEFINES})
target_include_directories(
  demo_text_tools_rpc_object
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  PRIVATE ${CANOPY_INCLUDES})
target_link_libraries(demo_text_tools_rpc_object PUBLIC text_tools_idl rpc::modules ${CANOPY_LIBRARIES})
target_compile_options(demo_text_tools_rpc_object PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(demo_text_tools_rpc_object PRIVATE ${CANOPY_LINK_OPTIONS})
set_property(TARGET demo_text_tools_rpc_object PROPERTY COMPILE_PDB_NAME demo_text_tools_rpc_object)

if(NOT CANOPY_BUILD_COROUTINE AND TARGET transport_blocking_dll_runtime)
  canopycreateblockingdll(
    demo_text_tools_blocking_dll
    SOURCES
    src/module.cpp
    LINK_LIBRARIES
    demo_text_tools_rpc_object
    text_tools_idl
    transport_local
    rpc_log)
endif()

if(CANOPY_BUILD_COROUTINE AND TARGET transport_shared_scheduler_dll_runtime)
  canopycreatesharedschedulerdll(
    demo_text_tools_coroutine_shared_dll
    SOURCES
    src/module.cpp
    LINK_LIBRARIES
    demo_text_tools_rpc_object
    text_tools_idl
    transport_local
    rpc_log)
endif()

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(demo_text_tools_rpc_object PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()
