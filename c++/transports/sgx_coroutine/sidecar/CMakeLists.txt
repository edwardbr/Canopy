#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

add_library(transport_sgx_coroutine_sidecar_common STATIC src/bootstrap.cpp)
target_compile_definitions(transport_sgx_coroutine_sidecar_common PRIVATE ${CANOPY_DEFINES} CANOPY_BUILD_ENCLAVE)
target_include_directories(
  transport_sgx_coroutine_sidecar_common
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
         "$<INSTALL_INTERFACE:include>"
  PRIVATE ${CANOPY_INCLUDES})
target_compile_options(transport_sgx_coroutine_sidecar_common PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(transport_sgx_coroutine_sidecar_common PRIVATE ${CANOPY_LINK_OPTIONS})
target_link_libraries(
  transport_sgx_coroutine_sidecar_common
  PUBLIC transport_secure_coroutine_module_headers secure_coroutine_module_idl streaming_spsc_queue rpc::rpc)
set_property(TARGET transport_sgx_coroutine_sidecar_common PROPERTY COMPILE_PDB_NAME
                                                                    transport_sgx_coroutine_sidecar_common)

add_executable(canopy_sgx_coroutine_sidecar src/main.cpp)
target_compile_definitions(
  canopy_sgx_coroutine_sidecar PRIVATE ${CANOPY_DEFINES} CANOPY_BUILD_ENCLAVE
                                       CANOPY_SGX_CREATE_ENCLAVE_DEBUG_FLAG=${CANOPY_SGX_CREATE_ENCLAVE_DEBUG_FLAG})
target_include_directories(canopy_sgx_coroutine_sidecar PRIVATE ${CANOPY_INCLUDES})
target_compile_options(canopy_sgx_coroutine_sidecar PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(canopy_sgx_coroutine_sidecar PRIVATE ${CANOPY_LINK_EXE_OPTIONS} ${CANOPY_SGX_HOST_LINK_OPTIONS})
target_link_directories(canopy_sgx_coroutine_sidecar PUBLIC ${CANOPY_SGX_HOST_LIBRARY_DIRS})
target_link_libraries(
  canopy_sgx_coroutine_sidecar
  PRIVATE transport_sgx_coroutine_sidecar_common
          transport_sgx_coroutine_host_headers
          yas_common
          secure_coroutine_module_idl
          rpc_types_idl
          streaming_spsc_queue
          ${CANOPY_LIBRARIES}
          ${CANOPY_SGX_HOST_LIBRARIES}
          rpc::rpc
          rpc_log
          sgx_coroutine_transport_edl)
set_property(TARGET canopy_sgx_coroutine_sidecar PROPERTY COMPILE_PDB_NAME canopy_sgx_coroutine_sidecar)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(transport_sgx_coroutine_sidecar_common PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
  set_target_properties(canopy_sgx_coroutine_sidecar PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()
