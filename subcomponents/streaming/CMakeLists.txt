#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

find_package(OpenSSL REQUIRED)

add_library(
  streaming SHARED
  src/tcp_stream.cpp
  src/tls_stream.cpp
  src/ws_stream.cpp)

target_include_directories(
  streaming
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  PRIVATE ${CANOPY_INCLUDES})

target_compile_definitions(streaming PRIVATE ${CANOPY_DEFINES})
target_compile_options(streaming PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(streaming PRIVATE ${CANOPY_LINK_EXE_OPTIONS})

target_link_libraries(
  streaming
  PUBLIC libcoro
         OpenSSL::Crypto
         OpenSSL::SSL
         wslay
         rpc
         canopy_network_config)

set_property(TARGET streaming PROPERTY COMPILE_PDB_NAME streaming)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(streaming PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()

message(STATUS "streaming configured")
