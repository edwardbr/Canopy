# Copyright (c) 2026 Edward Boggis-Rolfe All rights reserved.

cmake_minimum_required(VERSION 3.12)
project(websocket_server)

find_package(OpenSSL)

add_subdirectory(llama.cpp)

add_executable(
  websocket_server
  main.cpp
  demo.cpp
  http_client_connection.cpp
  rest_handler.cpp
  websocket_handler.cpp
  websocket_compat_stream.cpp
  demo_zone.cpp
  llama_server_engine.cpp)

target_compile_definitions(websocket_server PRIVATE ${CANOPY_DEFINES})

target_include_directories(
  websocket_server
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../../include>"
  PRIVATE ${CANOPY_INCLUDES})

target_link_libraries(
  websocket_server
  PUBLIC streaming
         transport_streaming
         streaming_transport_idl
         websocket_demo_idl
         secret_llama_idl
         llama
         common
         rpc
         canopy_http_server
         canopy_network_config
         ${CANOPY_LIBRARIES})

add_dependencies(websocket_server websocket_proto_js)

target_compile_options(websocket_server PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(websocket_server PRIVATE ${CANOPY_LINK_EXE_OPTIONS})

set_property(TARGET websocket_server PROPERTY COMPILE_PDB_NAME websocket_server)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(websocket_server PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()
