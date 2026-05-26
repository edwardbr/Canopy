#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

add_library(json INTERFACE)

target_include_directories(json INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
                                         "$<INSTALL_INTERFACE:include>")
target_link_libraries(json INTERFACE rpc::rpc yas_common)

message(STATUS "json configured")
