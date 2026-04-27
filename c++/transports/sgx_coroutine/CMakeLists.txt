#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

if(CANOPY_BUILD_ENCLAVE AND CANOPY_BUILD_COROUTINE)
  add_subdirectory(edl)

  add_library(transport_sgx_coroutine src/enclave_transport.cpp)

  target_compile_definitions(transport_sgx_coroutine PRIVATE ${CANOPY_DEFINES} CANOPY_BUILD_ENCLAVE)
  target_include_directories(
    transport_sgx_coroutine
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    PRIVATE ${CANOPY_INCLUDES})
  target_link_libraries(
    transport_sgx_coroutine
    PUBLIC spsc_queue streaming_spsc_queue transport_streaming
    PRIVATE yas_common
            canopy_coroutine_enclave_idl
            rpc_types_idl
            streaming_transport_idl
            streaming
            ${CANOPY_LIBRARIES}
            rpc::rpc
            canopy_coroutine_edl)
  target_compile_options(transport_sgx_coroutine PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
  target_link_options(transport_sgx_coroutine PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
  target_link_directories(transport_sgx_coroutine PUBLIC ${SGX_LIBRARY_PATH})
  set_property(TARGET transport_sgx_coroutine PROPERTY COMPILE_PDB_NAME transport_sgx_coroutine)

  if(CANOPY_ENABLE_CLANG_TIDY)
    set_target_properties(transport_sgx_coroutine PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
  endif()

  add_library(transport_sgx_coroutine_enclave src/enclave_runtime.cpp)
  target_compile_definitions(transport_sgx_coroutine_enclave PRIVATE ${CANOPY_ENCLAVE_DEFINES} CANOPY_BUILD_ENCLAVE)
  target_compile_options(transport_sgx_coroutine_enclave PRIVATE ${CANOPY_ENCLAVE_COMPILE_OPTIONS}
                                                                 ${CANOPY_WARN_OK_ENCLAVE})
  target_include_directories(
    transport_sgx_coroutine_enclave
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    PRIVATE ${CANOPY_ENCLAVE_LIBCXX_INCLUDES})
  target_link_libraries(
    transport_sgx_coroutine_enclave
    PUBLIC streaming_spsc_queue_enclave transport_streaming_enclave
    PRIVATE yas_common
            canopy_coroutine_enclave_idl_enclave
            rpc_types_idl_enclave
            streaming_transport_idl_enclave
            streaming_enclave
            spsc_queue
            libcoro_enclave
            rpc::rpc_enclave
            canopy_coroutine_edl_enclave)
  if(CANOPY_USE_TELEMETRY)
    target_link_libraries(transport_sgx_coroutine_enclave PRIVATE rpc::rpc_telemetry_enclave)
  endif()
  target_link_options(transport_sgx_coroutine_enclave PRIVATE ${CANOPY_ENCLAVE_LINK_OPTIONS})
  set_property(TARGET transport_sgx_coroutine_enclave PROPERTY COMPILE_PDB_NAME transport_sgx_coroutine_enclave)

  if(CANOPY_ENABLE_CLANG_TIDY)
    set_target_properties(transport_sgx_coroutine_enclave PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
  endif()

  add_library(transport_sgx_coroutine_test INTERFACE)
  target_include_directories(
    transport_sgx_coroutine_test INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/tests>"
                                           "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../sgx/tests>")
endif()
