#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

if(CANOPY_BUILD_ENCLAVE AND CANOPY_BUILD_COROUTINE)
  add_library(transport_sgx_coroutine host/src/transport.cpp)

  target_compile_definitions(
    transport_sgx_coroutine PRIVATE ${CANOPY_DEFINES} CANOPY_BUILD_ENCLAVE
                                    CANOPY_SGX_CREATE_ENCLAVE_DEBUG_FLAG=${CANOPY_SGX_CREATE_ENCLAVE_DEBUG_FLAG})
  target_link_libraries(
    transport_sgx_coroutine
    PUBLIC transport_sgx_coroutine_host_headers
           transport_secure_coroutine_module_headers
           spsc_queue
           streaming_spsc_queue
           transport_streaming
           canopy_io_uring
    PRIVATE yas_common
            secure_coroutine_module_idl
            rpc_types_idl
            streaming_transport_idl
            streaming
            ${CANOPY_LIBRARIES}
            ${CANOPY_SGX_HOST_LIBRARIES}
            rpc::rpc
            canopy_coroutine_edl)
  target_compile_options(transport_sgx_coroutine PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
  target_link_options(transport_sgx_coroutine PRIVATE ${CANOPY_LINK_EXE_OPTIONS} ${CANOPY_SGX_HOST_LINK_OPTIONS})
  target_link_directories(transport_sgx_coroutine PUBLIC ${CANOPY_SGX_HOST_LIBRARY_DIRS})
  set_property(TARGET transport_sgx_coroutine PROPERTY COMPILE_PDB_NAME transport_sgx_coroutine)

  if(CANOPY_ENABLE_CLANG_TIDY)
    set_target_properties(transport_sgx_coroutine PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
  endif()

  add_library(transport_sgx_coroutine_enclave_service_host_test STATIC enclave/src/service.cpp)
  target_compile_definitions(transport_sgx_coroutine_enclave_service_host_test PRIVATE ${CANOPY_DEFINES}
                                                                                       CANOPY_BUILD_ENCLAVE)
  target_compile_options(transport_sgx_coroutine_enclave_service_host_test PRIVATE ${CANOPY_COMPILE_OPTIONS}
                                                                                   ${CANOPY_WARN_OK})
  target_include_directories(transport_sgx_coroutine_enclave_service_host_test PRIVATE ${CANOPY_INCLUDES})
  target_link_libraries(
    transport_sgx_coroutine_enclave_service_host_test
    PUBLIC transport_sgx_coroutine_enclave_headers
           transport_secure_coroutine_module_headers
    PRIVATE canopy::security_attestation
            canopy_io_uring
            route_attestation_protocol_idl
            secure_coroutine_module_idl
            streaming
            transport_streaming
            rpc::rpc)
  target_link_options(transport_sgx_coroutine_enclave_service_host_test PRIVATE ${CANOPY_LINK_OPTIONS})
  set_property(TARGET transport_sgx_coroutine_enclave_service_host_test
               PROPERTY COMPILE_PDB_NAME transport_sgx_coroutine_enclave_service_host_test)

  if(CANOPY_ENABLE_CLANG_TIDY)
    set_target_properties(transport_sgx_coroutine_enclave_service_host_test PROPERTIES CXX_CLANG_TIDY
                                                                                       "${CLANG_TIDY_COMMAND}")
  endif()

  add_library(transport_sgx_coroutine_sgxssl_time_enclave STATIC enclave/src/sgxssl_time.cpp)
  target_compile_definitions(transport_sgx_coroutine_sgxssl_time_enclave PRIVATE ${CANOPY_ENCLAVE_DEFINES}
                                                                                 CANOPY_BUILD_ENCLAVE)
  target_compile_options(transport_sgx_coroutine_sgxssl_time_enclave PRIVATE ${CANOPY_ENCLAVE_COMPILE_OPTIONS}
                                                                             ${CANOPY_WARN_OK_ENCLAVE})
  target_include_directories(transport_sgx_coroutine_sgxssl_time_enclave PRIVATE ${CANOPY_ENCLAVE_LIBCXX_INCLUDES})
  target_link_libraries(
    transport_sgx_coroutine_sgxssl_time_enclave
    PUBLIC transport_sgx_coroutine_enclave_headers
    PRIVATE rpc::rpc_enclave)
  target_link_options(transport_sgx_coroutine_sgxssl_time_enclave PRIVATE ${CANOPY_ENCLAVE_LINK_OPTIONS})
  set_property(TARGET transport_sgx_coroutine_sgxssl_time_enclave PROPERTY CANOPY_SGX_WHOLE_ARCHIVE TRUE)
  set_property(TARGET transport_sgx_coroutine_sgxssl_time_enclave PROPERTY COMPILE_PDB_NAME
                                                                           transport_sgx_coroutine_sgxssl_time_enclave)

  add_library(transport_sgx_coroutine_enclave enclave/src/runtime.cpp enclave/src/host_transport.cpp
                                              enclave/src/io_uring_controller.cpp enclave/src/service.cpp)
  target_compile_definitions(transport_sgx_coroutine_enclave PRIVATE ${CANOPY_ENCLAVE_DEFINES} CANOPY_BUILD_ENCLAVE)
  target_compile_options(transport_sgx_coroutine_enclave PRIVATE ${CANOPY_ENCLAVE_COMPILE_OPTIONS}
                                                                 ${CANOPY_WARN_OK_ENCLAVE})
  target_include_directories(transport_sgx_coroutine_enclave PRIVATE ${CANOPY_ENCLAVE_LIBCXX_INCLUDES})
  target_link_libraries(
    transport_sgx_coroutine_enclave
    PUBLIC transport_sgx_coroutine_enclave_headers
           transport_secure_coroutine_module_headers
           streaming_spsc_queue_enclave
           transport_streaming_enclave
           canopy_io_uring_enclave
    PRIVATE yas_common
            secure_coroutine_module_idl_enclave
            rpc_types_idl_enclave
            route_attestation_protocol_idl_enclave
            streaming_transport_idl_enclave
            streaming_enclave
            spsc_queue
            libcoro_enclave
            canopy::security_attestation_enclave
            rpc::rpc_enclave
            transport_sgx_coroutine_sgxssl_time_enclave
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
