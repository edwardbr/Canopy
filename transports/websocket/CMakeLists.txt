#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

add_subdirectory(interface)

if(CANOPY_BUILD_COROUTINE)
  add_library(transport_websocket STATIC src/transport.cpp)

  target_include_directories(
    transport_websocket
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    PRIVATE ${CANOPY_INCLUDES})

  target_compile_definitions(transport_websocket PRIVATE ${CANOPY_DEFINES})
  target_compile_options(transport_websocket PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})

  target_link_libraries(transport_websocket PUBLIC websocket_protocol_idl streaming rpc)

  if(CANOPY_ENABLE_CLANG_TIDY)
    set_target_properties(transport_websocket PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
  endif()
endif()
