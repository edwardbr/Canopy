#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

include(GNUInstallDirs)

CanopyGenerate(
  connection_factory_config
  connection_factory_config/connection_factory_config.idl
  ${CMAKE_CURRENT_SOURCE_DIR}/interfaces
  ${CMAKE_BINARY_DIR}/generated
  ""
  yas_json
  dependencies rpc_types_idl json_idl stream_layers_idl
  include_paths ${CMAKE_CURRENT_SOURCE_DIR}/interfaces ${CANOPY_INTERFACE_SOURCE_DIR}
  install_dir ${GENERATED_INSTALL_DIR})

add_library(connection_factory STATIC src/application_config.cpp src/connection_factory.cpp src/options.cpp)
target_include_directories(
  connection_factory
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include>"
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CANOPY_INCLUDES})
target_compile_definitions(connection_factory PRIVATE ${CANOPY_DEFINES})
target_compile_definitions(connection_factory PRIVATE CANOPY_SCHEMA_ID_BASE="${CANOPY_SCHEMA_ID_BASE}")
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

add_executable(config_schema_compose tools/config_schema_compose.cpp)
target_include_directories(config_schema_compose PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CANOPY_INCLUDES})
target_compile_definitions(config_schema_compose PRIVATE ${CANOPY_DEFINES})
target_compile_definitions(config_schema_compose PRIVATE CANOPY_SCHEMA_ID_BASE="${CANOPY_SCHEMA_ID_BASE}")
target_compile_options(config_schema_compose PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(config_schema_compose PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
target_link_libraries(config_schema_compose PRIVATE connection_factory json ${CANOPY_LIBRARIES} rpc_log)
set_property(TARGET config_schema_compose PROPERTY COMPILE_PDB_NAME config_schema_compose)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(config_schema_compose PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()

if(TARGET streaming_tcp_blocking)
  target_sources(connection_factory PRIVATE src/adapters/streams/tcp_blocking.cpp)
  target_link_libraries(connection_factory PUBLIC streaming_tcp_blocking tcp_blocking_stream_config_idl)
  target_compile_definitions(connection_factory PUBLIC CANOPY_CONNECTION_FACTORY_HAS_TCP_BLOCKING)
endif()

install(
  DIRECTORY include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILES_MATCHING
  PATTERN "*.h*")

install(
  TARGETS connection_factory
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(TARGET transport_local)
  target_sources(connection_factory PRIVATE src/adapters/transports/local.cpp)
  target_link_libraries(connection_factory PUBLIC transport_local local_transport_config_idl)
  target_compile_definitions(connection_factory PUBLIC CANOPY_CONNECTION_FACTORY_HAS_LOCAL)
endif()

if(TARGET transport_blocking_dll AND NOT CANOPY_BUILD_COROUTINE)
  target_sources(connection_factory PRIVATE src/adapters/transports/blocking_dll.cpp)
  target_link_libraries(connection_factory PUBLIC transport_blocking_dll blocking_dll_config_idl)
  target_compile_definitions(connection_factory PUBLIC CANOPY_CONNECTION_FACTORY_HAS_BLOCKING_DLL)
endif()

if(TARGET transport_ipc_spsc)
  target_sources(connection_factory PRIVATE src/adapters/transports/ipc_spsc.cpp)
  target_link_libraries(connection_factory PUBLIC transport_ipc_spsc ipc_spsc_config_idl)
  target_compile_definitions(connection_factory PUBLIC CANOPY_CONNECTION_FACTORY_HAS_IPC_SPSC)
endif()

if(TARGET transport_shared_scheduler_dll)
  target_sources(connection_factory PRIVATE src/adapters/transports/shared_scheduler_dll.cpp)
  target_link_libraries(connection_factory PUBLIC transport_shared_scheduler_dll shared_dll_config_idl)
  target_compile_definitions(connection_factory PUBLIC CANOPY_CONNECTION_FACTORY_HAS_SHARED_SCHEDULER_DLL)
endif()

if(TARGET transport_unshared_scheduler_dll)
  target_sources(connection_factory PRIVATE src/adapters/transports/unshared_scheduler_dll.cpp)
  target_link_libraries(connection_factory PUBLIC transport_unshared_scheduler_dll unshared_dll_config_idl)
  target_compile_definitions(connection_factory PUBLIC CANOPY_CONNECTION_FACTORY_HAS_UNSHARED_SCHEDULER_DLL)
endif()

if(TARGET streaming_spsc_queue)
  target_sources(connection_factory PRIVATE src/adapters/streams/spsc_queue.cpp)
  target_link_libraries(connection_factory PUBLIC streaming_spsc_queue spsc_queue_stream_config_idl)
  target_compile_definitions(connection_factory PUBLIC CANOPY_CONNECTION_FACTORY_HAS_SPSC)
endif()

if(TARGET streaming_spsc_buffered_stream)
  target_link_libraries(connection_factory PUBLIC streaming_spsc_buffered_stream spsc_buffered_stream_config_idl)
  target_compile_definitions(connection_factory PUBLIC CANOPY_CONNECTION_FACTORY_HAS_SPSC_BUFFERED_STREAM)
endif()

if(TARGET streaming_tcp_coroutine)
  target_sources(connection_factory PRIVATE src/adapters/streams/tcp_coroutine.cpp)
  target_link_libraries(connection_factory PUBLIC streaming_tcp_coroutine)
  target_compile_definitions(connection_factory PUBLIC CANOPY_CONNECTION_FACTORY_HAS_TCP_COROUTINE)
endif()

if(CANOPY_BUILD_WEBSOCKET AND TARGET streaming_websocket)
  target_link_libraries(connection_factory PUBLIC streaming_websocket websocket_stream_config_idl)
  target_compile_definitions(connection_factory PUBLIC CANOPY_CONNECTION_FACTORY_HAS_WEBSOCKET)
endif()

if(TARGET streaming_openssl_tls)
  target_link_libraries(connection_factory PUBLIC streaming_openssl_tls openssl_tls_stream_config_idl)
  target_compile_definitions(connection_factory PUBLIC CANOPY_CONNECTION_FACTORY_HAS_TLS)
  target_compile_definitions(connection_factory PRIVATE CANOPY_CONNECTION_FACTORY_HAS_OPENSSL_TLS_CONFIG)
elseif(TARGET streaming_mbedtls)
  target_link_libraries(connection_factory PUBLIC streaming_mbedtls mbedtls_stream_config_idl)
  target_compile_definitions(connection_factory PUBLIC CANOPY_CONNECTION_FACTORY_HAS_TLS)
  target_compile_definitions(connection_factory PRIVATE CANOPY_CONNECTION_FACTORY_HAS_MBEDTLS_CONFIG)
endif()

if(TARGET streaming_compression)
  target_link_libraries(connection_factory PUBLIC streaming_compression compression_stream_config_idl)
  target_compile_definitions(connection_factory PUBLIC CANOPY_CONNECTION_FACTORY_HAS_COMPRESSION)
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

  if(TARGET streaming_spsc_buffered_stream)
    target_link_libraries(connection_factory_test INTERFACE streaming_spsc_buffered_stream)
  endif()

endif()
