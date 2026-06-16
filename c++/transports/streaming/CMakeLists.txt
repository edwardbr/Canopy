#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

CanopyGenerate(
  stream_transport_config
  stream_transport/stream_transport_config.idl
  ${CMAKE_CURRENT_SOURCE_DIR}/interfaces
  ${CMAKE_BINARY_DIR}/generated
  ""
  yas_json
  dependencies rpc_types_idl
  include_paths ${CMAKE_CURRENT_SOURCE_DIR}/interfaces ${CANOPY_INTERFACE_SOURCE_DIR}
  install_dir ${GENERATED_INSTALL_DIR})

# transport_streaming is dual-mode. Coroutine builds use libcoro; blocking builds dispatch the three loops to
# rpc::blocking_executor via the SPAWN macro and use rpc::event for completion signalling.
add_library(transport_streaming src/transport.cpp src/factory.cpp)

target_compile_definitions(transport_streaming PRIVATE ${CANOPY_DEFINES})

target_include_directories(
  transport_streaming
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include>"
  PRIVATE ${CANOPY_INCLUDES})

if(CANOPY_BUILD_COROUTINE)
  target_link_libraries(
    transport_streaming
    PUBLIC streaming json stream_transport_config_idl
    PRIVATE yas_common
            streaming_transport_idl
            rpc_types_idl
            libcoro
            ${CANOPY_LIBRARIES})
else()
  target_link_libraries(
    transport_streaming
    PUBLIC streaming json stream_transport_config_idl
    PRIVATE yas_common streaming_transport_idl rpc_types_idl ${CANOPY_LIBRARIES})
endif()

if(TARGET streaming_tcp_blocking)
  target_link_libraries(streaming_tcp_blocking PUBLIC transport_streaming)
endif()

if(TARGET streaming_spsc_queue)
  target_link_libraries(streaming_spsc_queue PUBLIC transport_streaming)
endif()

target_compile_options(transport_streaming PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(transport_streaming PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
set_property(TARGET transport_streaming PROPERTY COMPILE_PDB_NAME transport_streaming)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(transport_streaming PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()
