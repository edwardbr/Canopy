#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

#SGX transport requires enclave support
if(CANOPY_BUILD_ENCLAVE AND NOT CANOPY_BUILD_COROUTINE)
  CanopyGenerate(
    sgx_blocking_transport_config
    sgx_blocking_transport/sgx_blocking_transport_config.idl
    ${CMAKE_CURRENT_SOURCE_DIR}/interfaces
    ${CMAKE_BINARY_DIR}/generated
    ""
    yas_json
    dependencies rpc_types_idl json_idl sgx_enclave_runtime_config_idl
    include_paths ${CMAKE_CURRENT_SOURCE_DIR}/interfaces
                  ${CMAKE_CURRENT_SOURCE_DIR}/../sgx_common/interfaces
                  ${CMAKE_CURRENT_SOURCE_DIR}/../../subcomponents/io_uring_config/interfaces
                  ${CANOPY_INTERFACE_SOURCE_DIR}
    install_dir ${GENERATED_INSTALL_DIR})

  add_subdirectory(edl)

  add_library(transport_sgx_blocking src/transport.cpp src/factory.cpp)
  target_compile_definitions(
    transport_sgx_blocking PRIVATE ${CANOPY_DEFINES} CANOPY_BUILD_ENCLAVE
                                   CANOPY_SGX_CREATE_ENCLAVE_DEBUG_FLAG=${CANOPY_SGX_CREATE_ENCLAVE_DEBUG_FLAG})
  target_include_directories(
    transport_sgx_blocking
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    PRIVATE ${CANOPY_INCLUDES})
  target_link_libraries(
    transport_sgx_blocking
    PUBLIC transport_factory
           sgx_blocking_transport_config_idl
           json
    PRIVATE yas_common
            sgx_blocking_marshal_idl
            rpc_types_idl
            ${CANOPY_LIBRARIES}
            ${CANOPY_SGX_HOST_LIBRARIES}
            rpc::rpc
            sgx_blocking_transport_edl)
  target_compile_options(transport_sgx_blocking PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
  target_link_options(transport_sgx_blocking PRIVATE ${CANOPY_LINK_EXE_OPTIONS} ${CANOPY_SGX_HOST_LINK_OPTIONS})
  target_link_directories(transport_sgx_blocking PUBLIC ${CANOPY_SGX_HOST_LIBRARY_DIRS})
  set_property(TARGET transport_sgx_blocking PROPERTY COMPILE_PDB_NAME transport_sgx_blocking)

  if(CANOPY_ENABLE_CLANG_TIDY)
    set_target_properties(transport_sgx_blocking PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
  endif()

  add_library(transport_sgx_blocking_enclave src/host_transport.cpp)
  target_compile_definitions(transport_sgx_blocking_enclave PRIVATE ${CANOPY_ENCLAVE_DEFINES} CANOPY_BUILD_ENCLAVE)
  target_compile_options(transport_sgx_blocking_enclave PRIVATE ${CANOPY_ENCLAVE_COMPILE_OPTIONS} ${CANOPY_WARN_OK_ENCLAVE})
  target_include_directories(
    transport_sgx_blocking_enclave
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    PRIVATE ${CANOPY_ENCLAVE_LIBCXX_INCLUDES})
  target_link_libraries(
    transport_sgx_blocking_enclave
    PUBLIC sgx_blocking_transport_config_idl_enclave
           json_enclave
           rpc::modules_enclave
    PRIVATE yas_common
            sgx_blocking_marshal_idl_enclave
            rpc_types_idl_enclave
            rpc::rpc_enclave
            sgx_blocking_transport_edl_enclave)
  target_link_options(transport_sgx_blocking_enclave PRIVATE ${CANOPY_ENCLAVE_LINK_OPTIONS})
  set_property(TARGET transport_sgx_blocking_enclave PROPERTY COMPILE_PDB_NAME transport_sgx_blocking_enclave)

  if(CANOPY_ENABLE_CLANG_TIDY)
    set_target_properties(transport_sgx_blocking_enclave PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
  endif()

  add_library(transport_sgx_blocking_object_runtime_enclave STATIC src/object_runtime_exports.cpp)
  target_compile_definitions(transport_sgx_blocking_object_runtime_enclave PRIVATE ${CANOPY_ENCLAVE_DEFINES}
                                                                                   CANOPY_BUILD_ENCLAVE)
  target_compile_options(transport_sgx_blocking_object_runtime_enclave PRIVATE ${CANOPY_ENCLAVE_COMPILE_OPTIONS}
                                                                               ${CANOPY_WARN_OK_ENCLAVE})
  target_include_directories(
    transport_sgx_blocking_object_runtime_enclave
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    PRIVATE ${CANOPY_ENCLAVE_LIBCXX_INCLUDES})
  target_link_libraries(
    transport_sgx_blocking_object_runtime_enclave
    PUBLIC transport_sgx_blocking_enclave
           rpc::modules_enclave
    PRIVATE yas_common
            sgx_blocking_marshal_idl_enclave
            rpc_types_idl_enclave
            rpc::rpc_enclave
            sgx_blocking_transport_edl_enclave)
  set_property(TARGET transport_sgx_blocking_object_runtime_enclave PROPERTY COMPILE_PDB_NAME
                                                                             transport_sgx_blocking_object_runtime_enclave)

  if(CANOPY_ENABLE_CLANG_TIDY)
    set_target_properties(transport_sgx_blocking_object_runtime_enclave PROPERTIES CXX_CLANG_TIDY
                                                                                  "${CLANG_TIDY_COMMAND}")
  endif()

  add_library(transport_sgx_blocking_test INTERFACE)
  target_include_directories(
    transport_sgx_blocking_test INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/tests>")
endif()
