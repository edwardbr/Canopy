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
  dependencies rpc_types_idl tcp_config_idl io_uring_config_idl
  include_paths ${CMAKE_CURRENT_SOURCE_DIR}/interfaces
                ${CMAKE_SOURCE_DIR}/c++/streaming/tcp/interfaces
                ${CMAKE_SOURCE_DIR}/c++/subcomponents/io_uring_config/interfaces
                ${CANOPY_INTERFACE_SOURCE_DIR}
  install_dir ${GENERATED_INSTALL_DIR})

add_library(connection_factory INTERFACE)
target_include_directories(connection_factory INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
                                                        "$<INSTALL_INTERFACE:include>")
target_link_libraries(connection_factory INTERFACE transport_streaming streaming_tcp json connection_factory_config_idl
                                                tcp_config_idl io_uring_config_idl)

if(TARGET streaming_spsc_queue)
  target_link_libraries(connection_factory INTERFACE streaming_spsc_queue)
endif()

if(TARGET streaming_io_uring)
  target_link_libraries(connection_factory INTERFACE streaming_io_uring)
endif()

if(CANOPY_BUILD_COROUTINE)
  add_library(connection_factory_test INTERFACE)
  target_include_directories(connection_factory_test INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/tests>")
  target_link_libraries(connection_factory_test INTERFACE streaming streaming_tcp connection_factory)

  if(TARGET streaming_spsc_queue)
    target_link_libraries(connection_factory_test INTERFACE streaming_spsc_queue)
  endif()

  if(TARGET streaming_io_uring)
    target_link_libraries(connection_factory_test INTERFACE streaming_io_uring)
  endif()
endif()
