#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

CanopyGenerate(
  connection_factory_config
  connection_factory_config/connection_factory_config.idl
  ${CMAKE_CURRENT_SOURCE_DIR}/interfaces
  ${CMAKE_BINARY_DIR}/generated
  ""
  yas_json
  dependencies rpc_types_idl json_idl stream_layers_idl
  include_paths ${CMAKE_CURRENT_SOURCE_DIR}/interfaces
                ${CANOPY_INTERFACE_SOURCE_DIR}
  install_dir ${GENERATED_INSTALL_DIR})

add_library(
  connection_factory STATIC
  src/connection_factory.cpp
  src/options.cpp)
target_include_directories(
  connection_factory
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include>"
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CANOPY_INCLUDES})
target_compile_definitions(connection_factory PRIVATE ${CANOPY_DEFINES})
target_compile_options(connection_factory PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(connection_factory PRIVATE ${CANOPY_LINK_OPTIONS})
target_link_libraries(
  connection_factory
  PUBLIC transport_streaming
         streaming_layer_factory
         json
         connection_factory_config_idl
         json_idl
         stream_layers_idl
         io_uring_config_idl)

if(TARGET streaming_tcp_blocking)
  target_sources(
    connection_factory
    PRIVATE src/adapters/streams/tcp_blocking.cpp)
  target_link_libraries(connection_factory PUBLIC streaming_tcp_blocking tcp_blocking_stream_config_idl)
  target_compile_definitions(connection_factory PUBLIC CANOPY_CONNECTION_FACTORY_HAS_TCP_BLOCKING)
endif()

if(TARGET transport_local)
  target_sources(
    connection_factory
    PRIVATE src/adapters/transports/local.cpp)
  target_link_libraries(connection_factory PUBLIC transport_local local_transport_config_idl)
  target_compile_definitions(connection_factory PUBLIC CANOPY_CONNECTION_FACTORY_HAS_LOCAL)
endif()

if(TARGET transport_ipc_spsc)
  target_sources(
    connection_factory
    PRIVATE src/adapters/transports/ipc_spsc.cpp)
  target_link_libraries(connection_factory PUBLIC transport_ipc_spsc ipc_spsc_config_idl)
  target_compile_definitions(connection_factory PUBLIC CANOPY_CONNECTION_FACTORY_HAS_IPC_SPSC)
endif()

if(TARGET transport_shared_scheduler_dll)
  target_sources(
    connection_factory
    PRIVATE src/adapters/transports/shared_scheduler_dll.cpp)
  target_link_libraries(
    connection_factory
    PUBLIC transport_shared_scheduler_dll
           shared_dll_config_idl)
  target_compile_definitions(connection_factory PUBLIC CANOPY_CONNECTION_FACTORY_HAS_SHARED_SCHEDULER_DLL)
endif()

if(TARGET transport_unshared_scheduler_dll)
  target_sources(
    connection_factory
    PRIVATE src/adapters/transports/unshared_scheduler_dll.cpp)
  target_link_libraries(
    connection_factory
    PUBLIC transport_unshared_scheduler_dll
           unshared_dll_config_idl)
  target_compile_definitions(connection_factory PUBLIC CANOPY_CONNECTION_FACTORY_HAS_UNSHARED_SCHEDULER_DLL)
endif()

if(TARGET transport_sgx_coroutine)
  target_sources(
    connection_factory
    PRIVATE src/adapters/transports/sgx_coroutine.cpp)
  target_link_libraries(connection_factory PUBLIC transport_sgx_coroutine sgx_coroutine_transport_config_idl)
  target_compile_definitions(connection_factory PUBLIC CANOPY_CONNECTION_FACTORY_HAS_SGX_COROUTINE)
endif()

if(TARGET transport_sgx_blocking)
  target_sources(
    connection_factory
    PRIVATE src/adapters/transports/sgx_blocking.cpp)
  target_link_libraries(connection_factory PUBLIC transport_sgx_blocking sgx_blocking_transport_config_idl)
  target_compile_definitions(connection_factory PUBLIC CANOPY_CONNECTION_FACTORY_HAS_SGX_BLOCKING)
endif()

if(TARGET streaming_spsc_queue)
  target_sources(
    connection_factory
    PRIVATE src/adapters/streams/spsc_queue.cpp)
  target_link_libraries(connection_factory PUBLIC streaming_spsc_queue spsc_queue_stream_config_idl)
  target_compile_definitions(connection_factory PUBLIC CANOPY_CONNECTION_FACTORY_HAS_SPSC)
endif()

if(TARGET streaming_spsc_wrapping)
  target_link_libraries(connection_factory PUBLIC streaming_spsc_wrapping spsc_wrapping_stream_config_idl)
  target_compile_definitions(connection_factory PUBLIC CANOPY_CONNECTION_FACTORY_HAS_SPSC_WRAPPING)
endif()

if(TARGET streaming_tcp_coroutine)
  target_sources(
    connection_factory
    PRIVATE src/adapters/streams/tcp_coroutine.cpp)
  target_link_libraries(connection_factory PUBLIC streaming_tcp_coroutine)
  target_compile_definitions(connection_factory PUBLIC CANOPY_CONNECTION_FACTORY_HAS_TCP_COROUTINE)
endif()

if(CANOPY_BUILD_WEBSOCKET AND TARGET streaming_websocket)
  target_link_libraries(connection_factory PUBLIC streaming_websocket websocket_stream_config_idl)
  target_compile_definitions(connection_factory PUBLIC CANOPY_CONNECTION_FACTORY_HAS_WEBSOCKET)
endif()

if(TARGET streaming_tls)
  target_link_libraries(connection_factory PUBLIC streaming_tls tls_stream_config_idl)
  target_compile_definitions(connection_factory PUBLIC CANOPY_CONNECTION_FACTORY_HAS_TLS)
elseif(TARGET streaming_mbedtls)
  target_link_libraries(connection_factory PUBLIC streaming_mbedtls mbedtls_stream_config_idl)
  target_compile_definitions(connection_factory PUBLIC CANOPY_CONNECTION_FACTORY_HAS_TLS)
endif()

if(TARGET streaming_attestation)
  target_link_libraries(connection_factory PUBLIC streaming_attestation attestation_stream_config_idl)
  target_compile_definitions(connection_factory PUBLIC CANOPY_CONNECTION_FACTORY_HAS_ATTESTATION)
endif()

if(CANOPY_BUILD_COROUTINE)
  add_library(connection_factory_test INTERFACE)
  target_include_directories(connection_factory_test INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/tests>")
  target_link_libraries(connection_factory_test INTERFACE streaming connection_factory)

  if(TARGET streaming_tcp_blocking)
    target_link_libraries(connection_factory_test INTERFACE streaming_tcp_blocking)
  endif()

  if(TARGET streaming_spsc_queue)
    target_link_libraries(connection_factory_test INTERFACE streaming_spsc_queue)
  endif()

  if(TARGET streaming_tcp_coroutine)
    target_link_libraries(connection_factory_test INTERFACE streaming_tcp_coroutine)
  endif()

  if(TARGET streaming_spsc_wrapping)
    target_link_libraries(connection_factory_test INTERFACE streaming_spsc_wrapping)
  endif()

  if(TARGET transport_sgx_coroutine)
    target_link_libraries(connection_factory_test INTERFACE transport_sgx_coroutine)
  endif()
endif()
