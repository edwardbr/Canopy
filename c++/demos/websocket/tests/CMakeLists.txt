#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

if(NOT TARGET websocket_server_rest)
  message(STATUS "websocket demo tests disabled because websocket_server_rest is unavailable")
  return()
endif()

add_executable(websocket_rest_echo_test websocket_rest_echo_test.cpp)
target_compile_definitions(websocket_rest_echo_test PRIVATE ${CANOPY_DEFINES})
target_include_directories(websocket_rest_echo_test PRIVATE ${CANOPY_INCLUDES})
target_compile_options(websocket_rest_echo_test PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(websocket_rest_echo_test PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
target_link_libraries(
  websocket_rest_echo_test
  PRIVATE GTest::gtest_main
          websocket_server_rest
          websocket_rest_idl
          rest
          rest_connection_factory
          streaming
          streaming_http_client
          connection_factory
          json
          rpc_log)
add_test(NAME websocket_rest_echo_test COMMAND websocket_rest_echo_test)
