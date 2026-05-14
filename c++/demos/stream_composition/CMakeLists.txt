#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)
project(
  stream_composition_demo
  VERSION 1.0.0
  LANGUAGES CXX)

add_subdirectory(idl)

if(CANOPY_BUILD_COROUTINE)
  if(CANOPY_SECURE_STREAM_BACKEND STREQUAL "MBEDTLS")
    set(STREAM_COMPOSITION_SECURE_STREAM_TARGET streaming_mbedtls)
  elseif(CANOPY_SECURE_STREAM_BACKEND STREQUAL "OPENSSL")
    set(STREAM_COMPOSITION_SECURE_STREAM_TARGET streaming_tls)
  else()
    message(
      FATAL_ERROR "Unsupported CANOPY_SECURE_STREAM_BACKEND for stream_composition: ${CANOPY_SECURE_STREAM_BACKEND}")
  endif()

  if(NOT TARGET ${STREAM_COMPOSITION_SECURE_STREAM_TARGET})
    message(
      FATAL_ERROR
        "stream_composition requires ${STREAM_COMPOSITION_SECURE_STREAM_TARGET}; check CANOPY_SECURE_STREAM_BACKEND")
  endif()

  # The demo copies local fixture credentials into the binary tree at startup. Keeping fixtures local avoids
  # dependencies on the websocket demo or host-test data, while still letting the selected secure-stream backend own TLS
  # behaviour.
  set(DEMO_CERT_DIR "${CMAKE_CURRENT_BINARY_DIR}/certs")
  set(DEMO_CERT_FIXTURE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/certs")
  file(MAKE_DIRECTORY "${DEMO_CERT_DIR}")

  add_executable(tcp_spsc_tls_demo src/tcp_spsc_tls_demo.cpp)

  target_compile_definitions(tcp_spsc_tls_demo PRIVATE ${CANOPY_DEFINES} DEMO_CERT_DIR="${DEMO_CERT_DIR}"
                                                       DEMO_CERT_FIXTURE_DIR="${DEMO_CERT_FIXTURE_DIR}")

  target_include_directories(
    tcp_spsc_tls_demo
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    PRIVATE ${CANOPY_INCLUDES})

  target_link_libraries(
    tcp_spsc_tls_demo
    PUBLIC echo_idl
           streaming
           streaming_tcp
           ${STREAM_COMPOSITION_SECURE_STREAM_TARGET}
           streaming_spsc_wrapping
           transport_streaming
           rpc
           canopy_network_config
           ${CANOPY_LIBRARIES}
           rpc_log)

  target_compile_options(tcp_spsc_tls_demo PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
  target_link_options(tcp_spsc_tls_demo PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
  set_property(TARGET tcp_spsc_tls_demo PROPERTY COMPILE_PDB_NAME tcp_spsc_tls_demo)

  if(CANOPY_ENABLE_CLANG_TIDY)
    set_target_properties(tcp_spsc_tls_demo PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
  endif()

  message(STATUS "stream_composition demo configured")
  message(STATUS "  - tcp_spsc_tls_demo (TCP → SPSC → TLS composition, requires coroutines)")
  message(STATUS "  - Cert dir: ${DEMO_CERT_DIR}")
else()
  message(STATUS "stream_composition demo skipped (requires CANOPY_BUILD_COROUTINE)")
endif()
