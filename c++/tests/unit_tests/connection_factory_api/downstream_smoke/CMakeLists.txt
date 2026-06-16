#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

project(connection_factory_downstream_smoke LANGUAGES C CXX)

if(NOT DEFINED CANOPY_SOURCE_DIR)
  message(FATAL_ERROR "CANOPY_SOURCE_DIR must point at the Canopy repository root")
endif()

set(CANOPY_BUILD_TEST
    OFF
    CACHE BOOL "" FORCE)
set(CANOPY_BUILD_DEMOS
    OFF
    CACHE BOOL "" FORCE)
set(CANOPY_BUILD_BENCHMARKING
    OFF
    CACHE BOOL "" FORCE)
set(CANOPY_BUILD_RUST
    OFF
    CACHE BOOL "" FORCE)

add_subdirectory("${CANOPY_SOURCE_DIR}" canopy)

add_executable(downstream_connection_factory_smoke main.cpp)
target_link_libraries(downstream_connection_factory_smoke PRIVATE connection_factory)

add_library(downstream_connection_factory_bad_registration STATIC bad_registration.cpp)
target_link_libraries(downstream_connection_factory_bad_registration PRIVATE connection_factory)
