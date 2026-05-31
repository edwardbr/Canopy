#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

CanopyGenerate(
  untrusted_web_config
  untrusted_web/config.idl
  ${CMAKE_CURRENT_SOURCE_DIR}/interfaces
  ${CMAKE_BINARY_DIR}/generated
  ""
  yas_json
  dependencies rpc_types_idl
  include_paths ${CMAKE_CURRENT_SOURCE_DIR}/interfaces ${CANOPY_INTERFACE_SOURCE_DIR}
  install_dir ${GENERATED_INSTALL_DIR})

add_library(transport_untrusted_web STATIC src/transport.cpp src/factory.cpp)

target_include_directories(
  transport_untrusted_web
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated/include>"
  PRIVATE ${CANOPY_INCLUDES})

target_compile_definitions(transport_untrusted_web PRIVATE ${CANOPY_DEFINES})
target_compile_options(transport_untrusted_web PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})

target_link_libraries(
  transport_untrusted_web
  PUBLIC websocket_protocol_idl
         untrusted_web_config_idl
         streaming
         transport_factory
         rpc)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(transport_untrusted_web PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()

if(CANOPY_BUILD_COROUTINE)
  if(CANOPY_BUILD_ENCLAVE)
    add_library(transport_untrusted_web_enclave STATIC src/transport.cpp)

    target_include_directories(
      transport_untrusted_web_enclave
      PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated/include>"
      PRIVATE ${CANOPY_ENCLAVE_LIBCXX_INCLUDES})

    target_compile_definitions(transport_untrusted_web_enclave PRIVATE ${CANOPY_ENCLAVE_DEFINES})
    target_compile_options(transport_untrusted_web_enclave PRIVATE ${CANOPY_ENCLAVE_COMPILE_OPTIONS}
                                                               ${CANOPY_WARN_OK_ENCLAVE})
    target_link_options(transport_untrusted_web_enclave PRIVATE ${CANOPY_ENCLAVE_LINK_OPTIONS})
    target_link_libraries(
      transport_untrusted_web_enclave
      PUBLIC websocket_protocol_idl_enclave untrusted_web_config_idl_enclave streaming_enclave
      PRIVATE rpc::rpc_enclave)

    if(CANOPY_ENABLE_CLANG_TIDY)
      set_target_properties(transport_untrusted_web_enclave PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
    endif()
  endif()
endif()
