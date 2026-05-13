#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

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

  if(CANOPY_BUILD_ENCLAVE)
    add_library(transport_websocket_enclave STATIC src/transport.cpp)

    target_include_directories(
      transport_websocket_enclave
      PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
      PRIVATE ${CANOPY_ENCLAVE_LIBCXX_INCLUDES})

    target_compile_definitions(transport_websocket_enclave PRIVATE ${CANOPY_ENCLAVE_DEFINES})
    target_compile_options(transport_websocket_enclave PRIVATE ${CANOPY_ENCLAVE_COMPILE_OPTIONS}
                                                               ${CANOPY_WARN_OK_ENCLAVE})
    target_link_options(transport_websocket_enclave PRIVATE ${CANOPY_ENCLAVE_LINK_OPTIONS})
    target_link_libraries(
      transport_websocket_enclave
      PUBLIC websocket_protocol_idl_enclave streaming_enclave
      PRIVATE rpc::rpc_enclave)

    if(CANOPY_ENABLE_CLANG_TIDY)
      set_target_properties(transport_websocket_enclave PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
    endif()
  endif()
endif()
