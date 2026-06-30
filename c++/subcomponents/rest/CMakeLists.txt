#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

set(CANOPY_REST_SRCS src/helpers.cpp src/connection.cpp src/server.cpp)

if(TARGET canopy_http_server)
  list(APPEND CANOPY_REST_SRCS src/http_server_adapter.cpp)
endif()

add_library(rest STATIC ${CANOPY_REST_SRCS})

target_include_directories(
  rest
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include>"
  PRIVATE ${CANOPY_INCLUDES})

target_compile_definitions(rest PRIVATE ${CANOPY_DEFINES})
target_compile_options(rest PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(rest PRIVATE ${CANOPY_LINK_OPTIONS})
target_link_libraries(rest PUBLIC json streaming_http_client canopy_http_utils)

if(TARGET canopy_http_server)
  target_link_libraries(rest PUBLIC canopy_http_server)
endif()

add_library(canopy::rest ALIAS rest)

set_property(TARGET rest PROPERTY COMPILE_PDB_NAME rest)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(rest PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()

if(TARGET connection_factory)
  add_library(rest_connection_factory STATIC src/connection_factory.cpp)
  target_include_directories(
    rest_connection_factory
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include>"
    PRIVATE ${CANOPY_INCLUDES})

  target_compile_definitions(rest_connection_factory PRIVATE ${CANOPY_DEFINES})
  target_compile_options(rest_connection_factory PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
  target_link_options(rest_connection_factory PRIVATE ${CANOPY_LINK_OPTIONS})
  target_link_libraries(rest_connection_factory PUBLIC rest connection_factory)

  if(TARGET streaming_tcp_blocking)
    target_link_libraries(rest_connection_factory PUBLIC tcp_blocking_stream_config_idl)
    target_compile_definitions(rest_connection_factory PUBLIC CANOPY_REST_HAS_TCP_BLOCKING)
  endif()

  if(TARGET streaming_tcp_coroutine)
    target_link_libraries(rest_connection_factory PUBLIC tcp_coroutine_stream_config_idl)
    target_compile_definitions(rest_connection_factory PUBLIC CANOPY_REST_HAS_TCP_COROUTINE)
  endif()

  if(TARGET streaming_openssl_tls)
    target_link_libraries(rest_connection_factory PUBLIC openssl_tls_stream_config_idl)
    target_compile_definitions(rest_connection_factory PUBLIC CANOPY_REST_HAS_OPENSSL_TLS_CONFIG)
  elseif(TARGET streaming_mbedtls)
    target_link_libraries(rest_connection_factory PUBLIC mbedtls_stream_config_idl)
    target_compile_definitions(rest_connection_factory PUBLIC CANOPY_REST_HAS_MBEDTLS_CONFIG)
  endif()

  add_library(canopy::rest_connection_factory ALIAS rest_connection_factory)

  set_property(TARGET rest_connection_factory PROPERTY COMPILE_PDB_NAME rest_connection_factory)

  if(CANOPY_ENABLE_CLANG_TIDY)
    set_target_properties(rest_connection_factory PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
  endif()

  message(STATUS "rest_connection_factory configured")
endif()

message(STATUS "rest configured")
