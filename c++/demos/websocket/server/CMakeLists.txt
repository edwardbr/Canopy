# Copyright (c) 2026 Edward Boggis-Rolfe All rights reserved.

cmake_minimum_required(VERSION 3.12)
project(websocket_server)

if(NOT TARGET canopy_file_system)
  message(STATUS "websocket_server disabled because canopy_file_system is unavailable")
  return()
endif()

set(WEBSOCKET_DEMO_SECURE_STREAM_TARGET)
# Secure stream is currently coroutine-only. In blocking builds the demo runs over plain HTTP only; --cert/--key are
# rejected at runtime.
if(CANOPY_BUILD_COROUTINE)
  if(CANOPY_SECURE_STREAM_BACKEND STREQUAL "MBEDTLS")
    if(NOT TARGET streaming_mbedtls)
      message(FATAL_ERROR "websocket_server requires streaming_mbedtls when CANOPY_SECURE_STREAM_BACKEND=MBEDTLS")
    endif()
    set(WEBSOCKET_DEMO_SECURE_STREAM_TARGET streaming_mbedtls)
  elseif(CANOPY_SECURE_STREAM_BACKEND STREQUAL "OPENSSL")
    if(NOT TARGET streaming_openssl_tls)
      message(FATAL_ERROR "websocket_server requires streaming_openssl_tls when CANOPY_SECURE_STREAM_BACKEND=OPENSSL")
    endif()
    set(WEBSOCKET_DEMO_SECURE_STREAM_TARGET streaming_openssl_tls)
  else()
    message(FATAL_ERROR "Unsupported CANOPY_SECURE_STREAM_BACKEND=${CANOPY_SECURE_STREAM_BACKEND}")
  endif()
endif()

set(WEBSOCKET_DEMO_OPTIONAL_SRCS)
set(WEBSOCKET_DEMO_OPTIONAL_LIBS)
set(WEBSOCKET_DEMO_OPTIONAL_DEFINES)

if(NOT CANOPY_BUILD_COROUTINE AND NOT CANOPY_WEBSOCKET_DEMO_CALCULATOR_ONLY)
  message(STATUS "Forcing CANOPY_WEBSOCKET_DEMO_CALCULATOR_ONLY=ON for non-coroutine websocket_server")
  set(CANOPY_WEBSOCKET_DEMO_CALCULATOR_ONLY ON)
endif()

if(CANOPY_WEBSOCKET_DEMO_CALCULATOR_ONLY)
  list(APPEND WEBSOCKET_DEMO_OPTIONAL_DEFINES CANOPY_WEBSOCKET_DEMO_CALCULATOR_ONLY)
else()
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

  list(APPEND WEBSOCKET_DEMO_OPTIONAL_SRCS llama_server_engine.cpp)
  list(
    APPEND
    WEBSOCKET_DEMO_OPTIONAL_LIBS
    secret_llama_idl
    llama
    common)
endif()

if(TARGET transport_untrusted_web)
  target_compile_definitions(transport_untrusted_web
                             PRIVATE CANOPY_WEBSOCKET_ENCODING=${CANOPY_WEBSOCKET_DEMO_ENCODING_VALUE})
endif()

# Video session / genie / face_track depend on libvpx and an async video pipeline that hasn't been dual-moded yet. Keep
# them in the coroutine build only; blocking builds get a slim REST + WS calculator demo.
set(WEBSOCKET_DEMO_VIDEO_SRCS)
set(WEBSOCKET_DEMO_VIDEO_DEFINES)
if(CANOPY_BUILD_COROUTINE)
  list(
    APPEND
    WEBSOCKET_DEMO_VIDEO_SRCS
    video_session.cpp
    genie_sprite.cpp
    face_track.cpp)
  list(APPEND WEBSOCKET_DEMO_VIDEO_DEFINES CANOPY_WEBSOCKET_DEMO_ENABLE_VIDEO)
endif()

add_library(websocket_server_rest STATIC rest_echo_service.cpp)
target_compile_definitions(websocket_server_rest PRIVATE ${CANOPY_DEFINES})
target_include_directories(
  websocket_server_rest
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
  PRIVATE ${CANOPY_INCLUDES})
target_compile_options(websocket_server_rest PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(websocket_server_rest PRIVATE ${CANOPY_LINK_OPTIONS})
target_link_libraries(websocket_server_rest PUBLIC websocket_rest_idl rest rpc rpc_log)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(websocket_server_rest PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()

add_executable(
  websocket_server
  main.cpp
  demo.cpp
  http_client_connection.cpp
  websocket_handler.cpp
  demo_zone.cpp
  ${WEBSOCKET_DEMO_VIDEO_SRCS}
  ${WEBSOCKET_DEMO_OPTIONAL_SRCS})

target_compile_definitions(
  websocket_server PRIVATE ${CANOPY_DEFINES} ${WEBSOCKET_DEMO_OPTIONAL_DEFINES} ${WEBSOCKET_DEMO_VIDEO_DEFINES}
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
         connection_factory
         streaming_transport_idl
         transport_untrusted_web
         websocket_demo_idl
         websocket_server_rest
         ${WEBSOCKET_DEMO_OPTIONAL_LIBS}
         rpc
         canopy_file_system
         canopy_http_server
         network_config
         ${CANOPY_LIBRARIES}
         rpc_log)

if(CANOPY_BUILD_COROUTINE AND TARGET streaming_tcp_coroutine)
  target_link_libraries(websocket_server PUBLIC streaming_tcp_coroutine)
endif()

# libvpx is provided centrally by c++/submodules/CMakeLists.txt (gated on demos + websocket). Link it only if that
# target exists.
if(TARGET canopy_libvpx)
  target_link_libraries(websocket_server PUBLIC canopy_libvpx)
endif()

target_compile_options(websocket_server PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(websocket_server PRIVATE ${CANOPY_LINK_EXE_OPTIONS})

set_property(TARGET websocket_server PROPERTY COMPILE_PDB_NAME websocket_server)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(websocket_server PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()
