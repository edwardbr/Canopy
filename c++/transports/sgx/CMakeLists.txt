#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

# SGX transport requires enclave support
if(CANOPY_BUILD_ENCLAVE)
  add_subdirectory(edl)

  add_library(transport_sgx src/transport.cpp)
  target_compile_definitions(transport_sgx PRIVATE ${CANOPY_DEFINES} CANOPY_BUILD_ENCLAVE)
  target_include_directories(
    transport_sgx
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    PRIVATE ${CANOPY_INCLUDES})
  target_link_libraries(
    transport_sgx
    PRIVATE yas_common
            enclave_marshal_test_idl
            rpc_types_idl
            ${CANOPY_LIBRARIES}
            rpc::rpc
            marshal_test_edl)
  target_compile_options(transport_sgx PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
  target_link_options(transport_sgx PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
  target_link_directories(transport_sgx PUBLIC ${SGX_LIBRARY_PATH})
  set_property(TARGET transport_sgx PROPERTY COMPILE_PDB_NAME transport_sgx)

  if(CANOPY_ENABLE_CLANG_TIDY)
    set_target_properties(transport_sgx PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
  endif()

  add_library(transport_sgx_enclave src/host_transport.cpp)
  target_compile_definitions(transport_sgx_enclave PRIVATE ${CANOPY_ENCLAVE_DEFINES} CANOPY_BUILD_ENCLAVE)
  target_compile_options(transport_sgx_enclave PRIVATE ${CANOPY_ENCLAVE_COMPILE_OPTIONS} ${CANOPY_WARN_OK_ENCLAVE})
  target_include_directories(
    transport_sgx_enclave
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    PRIVATE ${CANOPY_ENCLAVE_LIBCXX_INCLUDES})
  target_link_libraries(
    transport_sgx_enclave
    PRIVATE yas_common
            enclave_marshal_test_idl_enclave
            rpc_types_idl_enclave
            rpc::rpc_enclave
            marshal_test_edl_enclave)
  target_link_options(transport_sgx_enclave PRIVATE ${CANOPY_ENCLAVE_LINK_OPTIONS})
  set_property(TARGET transport_sgx_enclave PROPERTY COMPILE_PDB_NAME transport_sgx_enclave)

  if(CANOPY_ENABLE_CLANG_TIDY)
    set_target_properties(transport_sgx_enclave PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
  endif()

  add_library(transport_sgx_test INTERFACE)
  target_include_directories(transport_sgx_test INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/tests>")
endif()
