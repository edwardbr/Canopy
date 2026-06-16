#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

set(JSON_INCLUDE_DIRECTORIES "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include>")

add_library(json INTERFACE)

target_include_directories(json INTERFACE ${JSON_INCLUDE_DIRECTORIES})
target_link_libraries(json INTERFACE rpc::rpc yas_common ${CANOPY_FMT_LIB})

message(STATUS "json configured")
