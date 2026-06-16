#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

add_library(rpc_modules INTERFACE)
add_library(rpc::modules ALIAS rpc_modules)

target_include_directories(
  rpc_modules
  INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
            "$<INSTALL_INTERFACE:include>")

target_link_libraries(rpc_modules INTERFACE rpc::rpc json)

install(
  DIRECTORY include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILES_MATCHING
  PATTERN "*.h*")

message(STATUS "modules configured")
