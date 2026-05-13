# Copyright (c) 2026 Edward Boggis-Rolfe All rights reserved.

cmake_minimum_required(VERSION 3.12)
project(websocket_server)

if(NOT TARGET canopy_file_system)
  message(STATUS "websocket_server disabled because canopy_file_system is unavailable")
  return()
endif()

set(WEBSOCKET_DEMO_SECURE_STREAM_TARGET)
if(CANOPY_SECURE_STREAM_BACKEND STREQUAL "MBEDTLS")
  if(NOT TARGET streaming_mbedtls)
    message(FATAL_ERROR "websocket_server requires streaming_mbedtls when CANOPY_SECURE_STREAM_BACKEND=MBEDTLS")
  endif()
  set(WEBSOCKET_DEMO_SECURE_STREAM_TARGET streaming_mbedtls)
elseif(CANOPY_SECURE_STREAM_BACKEND STREQUAL "OPENSSL")
  if(NOT TARGET streaming_tls)
    message(FATAL_ERROR "websocket_server requires streaming_tls when CANOPY_SECURE_STREAM_BACKEND=OPENSSL")
  endif()
  set(WEBSOCKET_DEMO_SECURE_STREAM_TARGET streaming_tls)
else()
  message(FATAL_ERROR "Unsupported CANOPY_SECURE_STREAM_BACKEND=${CANOPY_SECURE_STREAM_BACKEND}")
endif()

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

if(TARGET transport_websocket)
  target_compile_definitions(transport_websocket
                             PRIVATE CANOPY_WEBSOCKET_ENCODING=${CANOPY_WEBSOCKET_DEMO_ENCODING_VALUE})
endif()

add_executable(
  websocket_server
  main.cpp
  demo.cpp
  http_client_connection.cpp
  rest_handler.cpp
  websocket_handler.cpp
  demo_zone.cpp
  llama_server_engine.cpp)

target_compile_definitions(websocket_server PRIVATE ${CANOPY_DEFINES}
                                                    CANOPY_WEBSOCKET_DEMO_STATIC_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/www")

target_include_directories(
  websocket_server
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../../include>"
  PRIVATE ${CANOPY_INCLUDES})

target_link_libraries(
  websocket_server
  PUBLIC streaming
         ${WEBSOCKET_DEMO_SECURE_STREAM_TARGET}
         transport_streaming
         streaming_transport_idl
         transport_websocket
         websocket_demo_idl
         secret_llama_idl
         llama
         common
         rpc
         canopy_file_system
         canopy_http_server
         canopy_network_config
         ${CANOPY_LIBRARIES}
         rpc_log)

target_compile_options(websocket_server PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(websocket_server PRIVATE ${CANOPY_LINK_EXE_OPTIONS})

set_property(TARGET websocket_server PROPERTY COMPILE_PDB_NAME websocket_server)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(websocket_server PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()
