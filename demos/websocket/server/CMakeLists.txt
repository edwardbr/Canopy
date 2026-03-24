# Copyright (c) 2026 Edward Boggis-Rolfe All rights reserved.

cmake_minimum_required(VERSION 3.12)
project(websocket_server)

find_package(OpenSSL)

set(LLAMA_ALL_WARNINGS
    OFF
    CACHE BOOL "llama: enable all compiler warnings" FORCE)
set(LLAMA_FATAL_WARNINGS
    OFF
    CACHE BOOL "llama: enable -Werror flag" FORCE)
add_subdirectory(llama.cpp)

foreach(llama_demo_target IN ITEMS llama common cpp-httplib build_info)
  if(TARGET ${llama_demo_target})
    set_target_properties(${llama_demo_target} PROPERTIES CXX_CLANG_TIDY "")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      target_compile_options(${llama_demo_target} PRIVATE -w)
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
      target_compile_options(${llama_demo_target} PRIVATE -w)
    elseif(MSVC)
      target_compile_options(${llama_demo_target} PRIVATE /W0)
    endif()
  endif()
endforeach()

add_executable(
  websocket_server
  main.cpp
  demo.cpp
  http_client_connection.cpp
  rest_handler.cpp
  websocket_handler.cpp
  demo_zone.cpp
  transport.cpp
  address_translator.cpp
  llama_server_engine.cpp)

target_compile_definitions(websocket_server PRIVATE ${CANOPY_DEFINES})

target_include_directories(
  websocket_server
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../../include>"
  PRIVATE ${CANOPY_INCLUDES})

target_link_libraries(
  websocket_server
  PUBLIC streaming
         streaming_tls
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

target_compile_options(websocket_server PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(websocket_server PRIVATE ${CANOPY_LINK_EXE_OPTIONS})

set_property(TARGET websocket_server PROPERTY COMPILE_PDB_NAME websocket_server)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(websocket_server PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()
