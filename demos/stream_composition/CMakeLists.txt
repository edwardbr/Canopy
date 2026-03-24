#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)
project(
  stream_composition_demo
  VERSION 1.0.0
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(idl)

if(CANOPY_BUILD_COROUTINE)
  find_package(OpenSSL REQUIRED)

  # Directory where the demo writes its self-signed test certificates.
  set(DEMO_CERT_DIR "${CMAKE_CURRENT_BINARY_DIR}/certs")
  file(MAKE_DIRECTORY "${DEMO_CERT_DIR}")

  add_executable(tcp_spsc_tls_demo src/tcp_spsc_tls_demo.cpp)

  target_compile_definitions(tcp_spsc_tls_demo PRIVATE ${CANOPY_DEFINES} DEMO_CERT_DIR="${DEMO_CERT_DIR}")

  target_include_directories(
    tcp_spsc_tls_demo
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    PRIVATE ${CANOPY_INCLUDES})

  target_link_libraries(
    tcp_spsc_tls_demo
    PUBLIC echo_idl
           streaming
           streaming_tcp
           streaming_tls
           streaming_spsc_wrapping
           transport_streaming
           rpc
           canopy_network_config
           OpenSSL::Crypto
           OpenSSL::SSL
           ${CANOPY_LIBRARIES})

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
