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
if(TARGET transport_websocket_enclave)
  target_compile_definitions(transport_websocket_enclave
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
  video_session.cpp
  genie_sprite.cpp
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

# libvpx is provided centrally by c++/submodules/CMakeLists.txt (gated on
# demos + websocket). Link it only if that target exists.
if(TARGET canopy_libvpx)
  target_link_libraries(websocket_server PUBLIC canopy_libvpx)
endif()

target_compile_options(websocket_server PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(websocket_server PRIVATE ${CANOPY_LINK_EXE_OPTIONS})

set_property(TARGET websocket_server PROPERTY COMPILE_PDB_NAME websocket_server)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(websocket_server PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()

set(WEBSOCKET_DEMO_ENCLAVE_SECURE_STREAM_TARGET)
if(CANOPY_SECURE_STREAM_BACKEND STREQUAL "MBEDTLS")
  if(TARGET streaming_mbedtls_enclave)
    set(WEBSOCKET_DEMO_ENCLAVE_SECURE_STREAM_TARGET streaming_mbedtls_enclave)
  endif()
elseif(TARGET streaming_tls_enclave)
  set(WEBSOCKET_DEMO_ENCLAVE_SECURE_STREAM_TARGET streaming_tls_enclave)
endif()

if(CANOPY_BUILD_ENCLAVE
   AND CANOPY_BUILD_COROUTINE
   AND TARGET canopy_http_server_enclave
   AND TARGET canopy_file_system_enclave
   AND TARGET streaming_io_uring_enclave
   AND TARGET transport_websocket_enclave
   AND WEBSOCKET_DEMO_ENCLAVE_SECURE_STREAM_TARGET)
  set(WEBSOCKET_DEMO_ENCLAVE_SRCS
      enclave_websocket_server.cpp
      demo.cpp
      http_client_connection.cpp
      rest_handler.cpp
      websocket_handler.cpp
      video_session.cpp
      genie_sprite.cpp)

  add_library(websocket_demo_enclave_compile_fit STATIC ${WEBSOCKET_DEMO_ENCLAVE_SRCS})
  target_compile_definitions(
    websocket_demo_enclave_compile_fit
    PRIVATE ${CANOPY_ENCLAVE_DEFINES} CANOPY_WEBSOCKET_DEMO_CALCULATOR_ONLY
            CANOPY_WEBSOCKET_ENCODING=${CANOPY_WEBSOCKET_DEMO_ENCODING_VALUE}
            CANOPY_WEBSOCKET_DEMO_STATIC_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/www")
  target_include_directories(websocket_demo_enclave_compile_fit PRIVATE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
                                                                        ${CANOPY_ENCLAVE_LIBCXX_INCLUDES})
  target_compile_options(websocket_demo_enclave_compile_fit PRIVATE ${CANOPY_ENCLAVE_COMPILE_OPTIONS}
                                                                    ${CANOPY_WARN_OK_ENCLAVE})
  target_link_options(websocket_demo_enclave_compile_fit PRIVATE ${CANOPY_ENCLAVE_LINK_OPTIONS})
  target_link_libraries(
    websocket_demo_enclave_compile_fit
    PUBLIC websocket_demo_idl_enclave
           canopy_file_system_enclave
           canopy_http_server_enclave
           streaming_io_uring_enclave
           ${WEBSOCKET_DEMO_ENCLAVE_SECURE_STREAM_TARGET}
           streaming_websocket_enclave
           transport_websocket_enclave
           transport_sgx_coroutine_enclave
    PRIVATE rpc::rpc_enclave ${CANOPY_ENCLAVE_FMT_LIB})

  # libvpx built against the enclave freestanding toolchain — propagates the
  # vpx/ include dirs so video_session/genie_sprite compile under -nostdinc.
  if(TARGET canopy_libvpx_enclave)
    target_link_libraries(websocket_demo_enclave_compile_fit PRIVATE canopy_libvpx_enclave)
  endif()

  if((CANOPY_SECURE_STREAM_BACKEND STREQUAL "MBEDTLS" AND TARGET streaming_mbedtls_enclave)
     OR (CANOPY_SECURE_STREAM_BACKEND STREQUAL "OPENSSL" AND CANOPY_STREAMING_TLS_ENCLAVE_HAS_SGXSSL_IMPL))
    set(WEBSOCKET_DEMO_TELEMETRY_ENCLAVE_LIBS)
    if(CANOPY_USE_TELEMETRY)
      list(APPEND WEBSOCKET_DEMO_TELEMETRY_ENCLAVE_LIBS rpc_telemetry_enclave)
    endif()

    set(WEBSOCKET_DEMO_LIBVPX_ENCLAVE_LIB)
    if(TARGET canopy_libvpx_enclave)
      set(WEBSOCKET_DEMO_LIBVPX_ENCLAVE_LIB canopy_libvpx_enclave)
    endif()

    set(WEBSOCKET_DEMO_PROTOBUF_ENCLAVE_LIBS)
    if(TARGET canopy_nanopb_runtime_enclave)
      list(APPEND WEBSOCKET_DEMO_PROTOBUF_ENCLAVE_LIBS canopy_nanopb_runtime_enclave)
    endif()
    if(TARGET rpc_types_idl_enclave AND (CANOPY_BUILD_PROTOCOL_BUFFERS OR CANOPY_BUILD_NANOPB))
      list(APPEND WEBSOCKET_DEMO_PROTOBUF_ENCLAVE_LIBS rpc_types_idl_enclave)
    endif()

    add_enclave_library(
      websocket_demo_enclave
      SRCS ${WEBSOCKET_DEMO_ENCLAVE_SRCS}
      HEADER_ONLY_LIBS ${CANOPY_ENCLAVE_FMT_LIB}
      TRUSTED_LIBS
        libcoro_enclave
        rpc_enclave
        streaming_enclave
        streaming_io_uring_enclave
        ${WEBSOCKET_DEMO_ENCLAVE_SECURE_STREAM_TARGET}
        streaming_websocket_enclave
        transport_streaming_enclave
        streaming_transport_idl_enclave
        transport_websocket_enclave
        transport_sgx_coroutine_enclave
        ${WEBSOCKET_DEMO_TELEMETRY_ENCLAVE_LIBS}
        streaming_spsc_queue_enclave
        canopy_coroutine_edl_enclave
        canopy_io_uring_enclave
        canopy_file_system_enclave
        file_system_idl_enclave
        canopy_http_server_enclave
        websocket_demo_idl_enclave
        websocket_protocol_idl_enclave
        yas_common
        ${WEBSOCKET_DEMO_LIBVPX_ENCLAVE_LIB}
        ${WEBSOCKET_DEMO_PROTOBUF_ENCLAVE_LIBS}
        ${SGX_ENCLAVE_LIBS})

    target_compile_definitions(
      websocket_demo_enclave
      PRIVATE ${CANOPY_ENCLAVE_DEFINES} CANOPY_WEBSOCKET_DEMO_CALCULATOR_ONLY
              CANOPY_WEBSOCKET_ENCODING=${CANOPY_WEBSOCKET_DEMO_ENCODING_VALUE}
              CANOPY_WEBSOCKET_DEMO_STATIC_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/www")
    target_include_directories(websocket_demo_enclave PRIVATE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
                                                              ${CANOPY_ENCLAVE_LIBCXX_INCLUDES})
    target_compile_options(websocket_demo_enclave PRIVATE ${CANOPY_ENCLAVE_COMPILE_OPTIONS} ${CANOPY_WARN_OK_ENCLAVE})
    target_link_options(websocket_demo_enclave PRIVATE ${CANOPY_ENCLAVE_LINK_OPTIONS})
    set_property(TARGET websocket_demo_enclave PROPERTY COMPILE_PDB_NAME websocket_demo_enclave)

    if(CANOPY_ENABLE_CLANG_TIDY)
      set_target_properties(websocket_demo_enclave PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
    endif()

    enclave_sign(
      websocket_demo_enclave
      KEY ../../../tests/test_enclave/marshal_test_enclave_private.pem
      CONFIG ../../../tests/test_enclave/marshal_test_enclave.config.${SGX_MODE}.xml)

    # sgx_create_enclave expects the signed image in simulation and hardware modes.
    set(WEBSOCKET_DEMO_ENCLAVE_RUNTIME_PATH "${websocket_demo_enclave_sign_OUTPUT_NAME}")

    add_executable(websocket_enclave_server enclave_host_main.cpp)
    target_compile_definitions(
      websocket_enclave_server PRIVATE ${CANOPY_DEFINES}
                                       CANOPY_WEBSOCKET_DEMO_ENCLAVE_PATH="${WEBSOCKET_DEMO_ENCLAVE_RUNTIME_PATH}")
    target_include_directories(websocket_enclave_server PRIVATE ${CANOPY_INCLUDES})
    target_link_libraries(
      websocket_enclave_server
      PUBLIC websocket_demo_idl
             rpc::rpc
             transport_sgx_coroutine
             canopy_io_uring
             ${CANOPY_LIBRARIES}
             ${CANOPY_SGX_HOST_LIBRARIES}
             rpc_log)
    target_compile_options(websocket_enclave_server PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
    target_link_options(websocket_enclave_server PRIVATE ${CANOPY_LINK_EXE_OPTIONS} ${CANOPY_SGX_HOST_LINK_OPTIONS})
    target_link_directories(websocket_enclave_server PUBLIC ${SGX_LIBRARY_PATH})
    add_dependencies(websocket_enclave_server websocket_demo_enclave)

    if(CANOPY_ENABLE_CLANG_TIDY)
      set_target_properties(websocket_enclave_server PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
    endif()
  elseif(CANOPY_SECURE_STREAM_BACKEND STREQUAL "MBEDTLS")
    message(
      STATUS
        "websocket_demo_enclave compile-fit target configured with Mbed TLS, but runnable enclave packaging is disabled "
        "because streaming_mbedtls_enclave is not configured.")
  elseif(CANOPY_SGX_BACKEND STREQUAL "Fake")
    message(
      STATUS
        "websocket_demo_enclave compile-fit target configured, but runnable TLS enclave target is disabled because "
        "Fake SGX does not use Intel SGXSSL.")
  else()
    message(
      FATAL_ERROR
        "websocket_demo_enclave requires TLS-capable SGXSSL archives. Leave CANOPY_BOOTSTRAP_SGXSSL=ON to prepare "
        "them from submodules/confidential-computing.sgx/external/sgxssl, or set CANOPY_SGXSSL_LIB_DIR/"
        "CANOPY_SGXSSL_INCLUDE_DIR to an existing SGXSSL installation.")
  endif()
endif()
