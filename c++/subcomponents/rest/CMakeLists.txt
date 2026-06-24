#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

add_library(rest STATIC src/helpers.cpp src/connection.cpp)

target_include_directories(
  rest
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include>"
  PRIVATE ${CANOPY_INCLUDES})

target_compile_definitions(rest PRIVATE ${CANOPY_DEFINES})
target_compile_options(rest PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(rest PRIVATE ${CANOPY_LINK_OPTIONS})
target_link_libraries(rest PUBLIC json streaming_http_client connection_factory)

if(TARGET streaming_tcp_blocking)
  target_link_libraries(rest PUBLIC tcp_blocking_stream_config_idl)
  target_compile_definitions(rest PUBLIC CANOPY_REST_HAS_TCP_BLOCKING)
endif()

if(TARGET streaming_tcp_coroutine)
  target_link_libraries(rest PUBLIC tcp_coroutine_stream_config_idl)
  target_compile_definitions(rest PUBLIC CANOPY_REST_HAS_TCP_COROUTINE)
endif()

if(TARGET streaming_openssl_tls)
  target_link_libraries(rest PUBLIC openssl_tls_stream_config_idl)
  target_compile_definitions(rest PUBLIC CANOPY_REST_HAS_OPENSSL_TLS_CONFIG)
elseif(TARGET streaming_mbedtls)
  target_link_libraries(rest PUBLIC mbedtls_stream_config_idl)
  target_compile_definitions(rest PUBLIC CANOPY_REST_HAS_MBEDTLS_CONFIG)
endif()

add_library(canopy::rest ALIAS rest)

set_property(TARGET rest PROPERTY COMPILE_PDB_NAME rest)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(rest PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()

message(STATUS "rest configured")
