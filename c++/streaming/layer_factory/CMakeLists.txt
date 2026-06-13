#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

add_library(streaming_layer_factory STATIC src/factory.cpp)
target_include_directories(
  streaming_layer_factory
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include>"
  PRIVATE ${CANOPY_INCLUDES})
target_compile_definitions(streaming_layer_factory PRIVATE ${CANOPY_DEFINES})
target_compile_options(streaming_layer_factory PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(streaming_layer_factory PRIVATE ${CANOPY_LINK_OPTIONS})
target_link_libraries(streaming_layer_factory PUBLIC streaming stream_layers_idl json)

if(TARGET streaming_websocket)
  target_compile_definitions(streaming_layer_factory PUBLIC CANOPY_STREAMING_LAYER_FACTORY_HAS_WEBSOCKET)
  target_link_libraries(streaming_layer_factory PUBLIC streaming_websocket websocket_stream_config_idl)
endif()

if(TARGET streaming_openssl_tls)
  target_compile_definitions(streaming_layer_factory PUBLIC CANOPY_STREAMING_LAYER_FACTORY_HAS_TLS
                                                            CANOPY_STREAMING_LAYER_FACTORY_HAS_OPENSSL_TLS_CONFIG)
  target_link_libraries(streaming_layer_factory PUBLIC streaming_openssl_tls openssl_tls_stream_config_idl)
elseif(TARGET streaming_mbedtls)
  target_compile_definitions(streaming_layer_factory PUBLIC CANOPY_STREAMING_LAYER_FACTORY_HAS_TLS
                                                            CANOPY_STREAMING_LAYER_FACTORY_HAS_MBEDTLS_CONFIG)
  target_link_libraries(streaming_layer_factory PUBLIC streaming_mbedtls mbedtls_stream_config_idl)
endif()

if(TARGET streaming_compression)
  target_compile_definitions(streaming_layer_factory PUBLIC CANOPY_STREAMING_LAYER_FACTORY_HAS_COMPRESSION)
  target_link_libraries(streaming_layer_factory PUBLIC streaming_compression compression_stream_config_idl)
endif()

if(TARGET streaming_spsc_buffered_stream)
  target_compile_definitions(streaming_layer_factory PUBLIC CANOPY_STREAMING_LAYER_FACTORY_HAS_SPSC_BUFFERED_STREAM)
  target_link_libraries(streaming_layer_factory PUBLIC streaming_spsc_buffered_stream spsc_buffered_stream_config_idl)
endif()

if(TARGET streaming_attestation)
  target_compile_definitions(streaming_layer_factory PUBLIC CANOPY_STREAMING_LAYER_FACTORY_HAS_ATTESTATION)
  target_link_libraries(streaming_layer_factory PUBLIC streaming_attestation attestation_stream_config_idl)
endif()

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(streaming_layer_factory PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()

if(CANOPY_BUILD_ENCLAVE)
  add_library(streaming_layer_factory_enclave STATIC src/factory.cpp)
  target_include_directories(
    streaming_layer_factory_enclave
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include>"
    PRIVATE ${CANOPY_ENCLAVE_LIBCXX_INCLUDES})
  target_compile_definitions(streaming_layer_factory_enclave PRIVATE ${CANOPY_ENCLAVE_DEFINES})
  target_compile_options(streaming_layer_factory_enclave PRIVATE ${CANOPY_ENCLAVE_COMPILE_OPTIONS}
                                                                 ${CANOPY_WARN_OK_ENCLAVE})
  target_link_options(streaming_layer_factory_enclave PRIVATE ${CANOPY_ENCLAVE_LINK_OPTIONS})
  target_link_libraries(streaming_layer_factory_enclave PUBLIC streaming_enclave stream_layers_idl_enclave json_enclave)
  if(TARGET streaming_websocket_enclave AND TARGET websocket_stream_config_idl_enclave)
    target_compile_definitions(streaming_layer_factory_enclave PUBLIC CANOPY_STREAMING_LAYER_FACTORY_HAS_WEBSOCKET)
    target_link_libraries(streaming_layer_factory_enclave PUBLIC streaming_websocket_enclave
                                                         websocket_stream_config_idl_enclave)
  endif()
  if(TARGET streaming_openssl_tls_enclave AND TARGET openssl_tls_stream_config_idl_enclave)
    target_compile_definitions(streaming_layer_factory_enclave PUBLIC CANOPY_STREAMING_LAYER_FACTORY_HAS_TLS
                                                                      CANOPY_STREAMING_LAYER_FACTORY_HAS_OPENSSL_TLS_CONFIG)
    target_link_libraries(streaming_layer_factory_enclave PUBLIC streaming_openssl_tls_enclave
                                                                 openssl_tls_stream_config_idl_enclave)
  endif()
  if(TARGET streaming_attestation_enclave AND TARGET attestation_stream_config_idl_enclave)
    target_compile_definitions(streaming_layer_factory_enclave PUBLIC CANOPY_STREAMING_LAYER_FACTORY_HAS_ATTESTATION)
    target_link_libraries(streaming_layer_factory_enclave PUBLIC streaming_attestation_enclave
                                                               attestation_stream_config_idl_enclave)
  endif()
  if(CANOPY_ENABLE_CLANG_TIDY)
    set_target_properties(streaming_layer_factory_enclave PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
  endif()
endif()
