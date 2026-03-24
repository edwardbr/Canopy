#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

find_package(OpenSSL REQUIRED)

add_library(canopy_http_server STATIC src/http_acceptor.cpp src/http_client_connection.cpp
                                      src/static_webpage_delivery.cpp)

target_include_directories(
  canopy_http_server
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  PRIVATE ${CANOPY_INCLUDES})

target_compile_definitions(canopy_http_server PRIVATE ${CANOPY_DEFINES})
target_compile_options(canopy_http_server PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(canopy_http_server PRIVATE ${CANOPY_LINK_EXE_OPTIONS})

target_link_libraries(
  canopy_http_server
  PUBLIC streaming
         streaming_tls
         transport_streaming
         llhttp_static
         rpc
  PRIVATE streaming_tcp
          streaming_websocket
          OpenSSL::SSL
          OpenSSL::Crypto
          ${CANOPY_FMT_LIB}
          ${CANOPY_LIBRARIES})

set_property(TARGET canopy_http_server PROPERTY COMPILE_PDB_NAME canopy_http_server)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(canopy_http_server PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()

message(STATUS "canopy_http_server configured")
