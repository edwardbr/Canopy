#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

# SGX transport requires enclave support
if(CANOPY_BUILD_ENCLAVE)
  add_subdirectory(edl)

  # Enclave service proxy library
  add_library(rpc_sgx_enclave_service_proxy src/enclave_service_proxy.cpp src/enclave_service_proxy.h)

  target_compile_definitions(rpc_sgx_enclave_service_proxy PRIVATE ${CANOPY_ENCLAVE_DEFINES})
  target_compile_options(rpc_sgx_enclave_service_proxy PRIVATE ${CANOPY_ENCLAVE_COMPILE_OPTIONS} ${CANOPY_WARN_OK})

  target_include_directories(
    rpc_sgx_enclave_service_proxy
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
    PRIVATE ${CANOPY_ENCLAVE_LIBCXX_INCLUDES})

  target_link_libraries(rpc_sgx_enclave_service_proxy PRIVATE yas_common rpc_types_idl_enclave rpc::rpc_enclave
                                                              marshal_test_edl_enclave)

  target_link_options(rpc_sgx_enclave_service_proxy PRIVATE ${CANOPY_ENCLAVE_LINK_OPTIONS})
  set_property(TARGET rpc_sgx_enclave_service_proxy PROPERTY COMPILE_PDB_NAME rpc_sgx_enclave_service_proxy)

  if(CANOPY_ENABLE_CLANG_TIDY)
    set_target_properties(rpc_sgx_enclave_service_proxy PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
  endif()

  # Host service proxy library
  add_library(rpc_sgx_service_proxy src/host_service_proxy.cpp src/host_service_proxy.h)

  target_compile_definitions(rpc_sgx_service_proxy PRIVATE ${CANOPY_DEFINES})

  target_include_directories(
    rpc_sgx_service_proxy
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
    PRIVATE ${CANOPY_INCLUDES})

  target_link_libraries(
    rpc_sgx_service_proxy
    PRIVATE yas_common
            rpc_types_idl
            ${CANOPY_LIBRARIES}
            rpc::rpc
            marshal_test_edl)

  target_compile_options(rpc_sgx_service_proxy PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
  target_link_options(rpc_sgx_service_proxy PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
  target_link_directories(rpc_sgx_service_proxy PUBLIC ${SGX_LIBRARY_PATH})
  set_property(TARGET rpc_sgx_service_proxy PROPERTY COMPILE_PDB_NAME rpc_sgx_service_proxy)

  if(CANOPY_ENABLE_CLANG_TIDY)
    set_target_properties(rpc_sgx_service_proxy PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
  endif()

  add_library(transport_sgx_test INTERFACE)
  target_include_directories(transport_sgx_test INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/tests>")
endif()
