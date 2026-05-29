#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

set(JSON_INCLUDE_DIRECTORIES "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include>")

add_library(json INTERFACE)

target_include_directories(json INTERFACE ${JSON_INCLUDE_DIRECTORIES})
target_link_libraries(json INTERFACE rpc::rpc yas_common ${CANOPY_FMT_LIB})

if(CANOPY_BUILD_ENCLAVE AND TARGET rpc::rpc_enclave)
  add_library(json_enclave INTERFACE)
  target_include_directories(json_enclave INTERFACE ${JSON_INCLUDE_DIRECTORIES})
  target_link_libraries(json_enclave INTERFACE rpc::rpc_enclave yas_common ${CANOPY_ENCLAVE_FMT_LIB})
endif()

message(STATUS "json configured")
