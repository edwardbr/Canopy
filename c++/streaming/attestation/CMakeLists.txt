#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

CanopyGenerate(
  attestation_stream_config
  attestation_stream/attestation_stream_config.idl
  ${CMAKE_CURRENT_SOURCE_DIR}/interfaces
  ${CMAKE_BINARY_DIR}/generated
  ""
  yas_json
  dependencies rpc_types_idl
  include_paths ${CMAKE_CURRENT_SOURCE_DIR}/interfaces ${CANOPY_INTERFACE_SOURCE_DIR}
  install_dir ${GENERATED_INSTALL_DIR})

add_library(streaming_attestation STATIC src/stream.cpp)
add_library(canopy::streaming_attestation ALIAS streaming_attestation)

target_include_directories(
  streaming_attestation
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include>"
  PRIVATE ${CANOPY_INCLUDES})
target_compile_definitions(streaming_attestation PRIVATE ${CANOPY_DEFINES})
target_compile_options(streaming_attestation PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(streaming_attestation PRIVATE ${CANOPY_LINK_OPTIONS})
target_link_libraries(streaming_attestation PUBLIC streaming canopy::security_attestation libcoro attestation_stream_config_idl)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(streaming_attestation PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()

if(CANOPY_BUILD_ENCLAVE
   AND CANOPY_BUILD_COROUTINE
   AND TARGET security_attestation_enclave)
  add_library(streaming_attestation_enclave STATIC src/stream.cpp)
  add_library(canopy::streaming_attestation_enclave ALIAS streaming_attestation_enclave)

  target_include_directories(
    streaming_attestation_enclave
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include>"
    PRIVATE ${CANOPY_ENCLAVE_LIBCXX_INCLUDES})
  target_compile_definitions(streaming_attestation_enclave PRIVATE ${CANOPY_ENCLAVE_DEFINES})
  target_compile_options(streaming_attestation_enclave PRIVATE ${CANOPY_ENCLAVE_COMPILE_OPTIONS}
                                                               ${CANOPY_WARN_OK_ENCLAVE})
  target_link_options(streaming_attestation_enclave PRIVATE ${CANOPY_ENCLAVE_LINK_OPTIONS})
  set(STREAMING_ATTESTATION_ENCLAVE_PUBLIC_LIBS streaming_enclave canopy::security_attestation_enclave libcoro_enclave)
  if(TARGET attestation_stream_config_idl_enclave)
    list(APPEND STREAMING_ATTESTATION_ENCLAVE_PUBLIC_LIBS attestation_stream_config_idl_enclave)
  endif()
  target_link_libraries(streaming_attestation_enclave PUBLIC ${STREAMING_ATTESTATION_ENCLAVE_PUBLIC_LIBS})

  if(CANOPY_ENABLE_CLANG_TIDY)
    set_target_properties(streaming_attestation_enclave PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
  endif()

  message(STATUS "streaming_attestation_enclave configured")
endif()

if(CANOPY_BUILD_TEST
   AND CMAKE_SYSTEM_NAME STREQUAL "Linux"
   AND CANOPY_BUILD_COROUTINE)
  add_executable(attestation_stream_test tests/attestation_stream_test.cpp)
  target_include_directories(attestation_stream_test PRIVATE ${CANOPY_INCLUDES})
  target_compile_definitions(attestation_stream_test PRIVATE ${CANOPY_DEFINES})
  target_compile_options(attestation_stream_test PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
  target_link_options(attestation_stream_test PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
  target_link_libraries(attestation_stream_test PRIVATE streaming_attestation streaming_spsc_queue GTest::gtest_main
                                                        rpc_log)
  add_test(NAME attestation_stream_test COMMAND attestation_stream_test)
  message(STATUS "attestation_stream_test configured")
endif()

message(STATUS "streaming_attestation configured")
