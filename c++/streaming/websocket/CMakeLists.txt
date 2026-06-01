#
# Copyright (c) 2026 Edward Boggis-Rolfe All rights reserved.
#

cmake_minimum_required(VERSION 3.24)

CanopyGenerate(
  websocket_stream_config
  websocket_stream/websocket_stream_config.idl
  ${CMAKE_CURRENT_SOURCE_DIR}/interfaces
  ${CMAKE_BINARY_DIR}/generated
  ""
  yas_json
  dependencies rpc_types_idl
  include_paths ${CMAKE_CURRENT_SOURCE_DIR}/interfaces ${CANOPY_INTERFACE_SOURCE_DIR}
  install_dir ${GENERATED_INSTALL_DIR})

add_library(streaming_websocket STATIC src/stream.cpp)
target_include_directories(
  streaming_websocket
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include>"
  PRIVATE ${CANOPY_INCLUDES})
target_compile_definitions(streaming_websocket PRIVATE ${CANOPY_DEFINES})
target_compile_options(streaming_websocket PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(streaming_websocket PRIVATE ${CANOPY_LINK_OPTIONS})
if(CANOPY_BUILD_COROUTINE)
  target_link_libraries(
    streaming_websocket
    PUBLIC streaming websocket_stream_config_idl
    PRIVATE wslay rpc libcoro)
else()
  # Blocking-mode WS stream operates over a dual-mode streaming::stream and the pure-C wslay library; no libcoro link.
  target_link_libraries(
    streaming_websocket
    PUBLIC streaming websocket_stream_config_idl
    PRIVATE wslay rpc)
endif()
if(CANOPY_BUILD_WEBSOCKET_PERMESSAGE_DEFLATE AND TARGET canopy_zlib)
  target_compile_definitions(streaming_websocket PUBLIC CANOPY_WEBSOCKET_HAS_PERMESSAGE_DEFLATE)
  target_link_libraries(streaming_websocket PRIVATE canopy_zlib)
endif()
if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(streaming_websocket PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()

if(CANOPY_BUILD_ENCLAVE)
  add_library(streaming_websocket_enclave STATIC src/stream.cpp)
  target_include_directories(
    streaming_websocket_enclave
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include>"
    PRIVATE ${CANOPY_ENCLAVE_LIBCXX_INCLUDES} "${CMAKE_SOURCE_DIR}/c++/submodules/wslay/lib/includes"
            "${CMAKE_BINARY_DIR}/c++/submodules/wslay/lib/includes" "${CMAKE_BINARY_DIR}/c++/submodules/wslay/lib")
  target_compile_definitions(streaming_websocket_enclave PRIVATE ${CANOPY_ENCLAVE_DEFINES})
  target_compile_options(streaming_websocket_enclave PRIVATE ${CANOPY_ENCLAVE_COMPILE_OPTIONS}
                                                             ${CANOPY_WARN_OK_ENCLAVE})
  target_link_options(streaming_websocket_enclave PRIVATE ${CANOPY_ENCLAVE_LINK_OPTIONS})
  target_link_libraries(
    streaming_websocket_enclave
    PUBLIC streaming_enclave
    PRIVATE wslay_enclave rpc::rpc_enclave)
  if(TARGET websocket_stream_config_idl_enclave)
    target_link_libraries(streaming_websocket_enclave PUBLIC websocket_stream_config_idl_enclave)
  endif()
  if(CANOPY_BUILD_WEBSOCKET_PERMESSAGE_DEFLATE AND TARGET canopy_zlib_enclave)
    target_compile_definitions(streaming_websocket_enclave PUBLIC CANOPY_WEBSOCKET_HAS_PERMESSAGE_DEFLATE)
    target_link_libraries(streaming_websocket_enclave PRIVATE canopy_zlib_enclave)
  endif()
  if(CANOPY_ENABLE_CLANG_TIDY)
    set_target_properties(streaming_websocket_enclave PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
  endif()
  message(STATUS "streaming_websocket_enclave configured")
endif()
