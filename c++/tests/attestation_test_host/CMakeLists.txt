#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]
cmake_minimum_required(VERSION 3.24)

if(CANOPY_BUILD_ENCLAVE AND CANOPY_BUILD_COROUTINE)
  add_executable(sgx_attestation_test_host sgx_attestation_test_host.cpp)

  if(WIN32)
    set(CANOPY_TEST_SGX_ATTESTATION_ENCLAVE_PATH
        "$<TARGET_FILE_DIR:sgx_attestation_test_enclave>/sgx_attestation_test_enclave.signed.dll")
  else()
    set(CANOPY_TEST_SGX_ATTESTATION_ENCLAVE_PATH
        "$<TARGET_FILE_DIR:sgx_attestation_test_enclave>/libsgx_attestation_test_enclave.signed.so")
  endif()

  target_compile_definitions(
    sgx_attestation_test_host
    PRIVATE ${CANOPY_DEFINES} CANOPY_TEST_SGX_ATTESTATION_ENCLAVE_PATH="${CANOPY_TEST_SGX_ATTESTATION_ENCLAVE_PATH}")
  target_include_directories(
    sgx_attestation_test_host
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated/include>"
           "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated/src>"
    PRIVATE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
            "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../sgx_coroutine_test_host>" ${CANOPY_INCLUDES})

  target_link_libraries(
    sgx_attestation_test_host
    PUBLIC attestation_test_idl
           sgx_sim_protocol_idl
           example_idl
           canopy::security_attestation
           rpc::rpc
           transport_sgx_coroutine
           transport_sgx_coroutine_test
           GTest::gtest_main
           fmt::fmt-header-only
           spdlog::spdlog
           ${CANOPY_FMT_LIB}
           ${CANOPY_LIBRARIES}
           ${CANOPY_SGX_HOST_LIBRARIES}
           ${CMAKE_DL_LIBS}
           rpc_log)

  target_compile_options(sgx_attestation_test_host PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
  target_link_options(sgx_attestation_test_host PRIVATE ${CANOPY_LINK_EXE_OPTIONS} ${CANOPY_SGX_HOST_LINK_OPTIONS})
  target_link_directories(sgx_attestation_test_host PUBLIC ${CANOPY_SGX_HOST_LIBRARY_DIRS})
  set_property(TARGET sgx_attestation_test_host PROPERTY COMPILE_PDB_NAME sgx_attestation_test_host)

  if(CANOPY_ENABLE_CLANG_TIDY)
    set_target_properties(sgx_attestation_test_host PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
  endif()

  add_dependencies(sgx_attestation_test_host sgx_attestation_test_enclave)

  include(CTest)
  if(NOT CANOPY_DEBUG_ADDRESS)
    gtest_discover_tests(sgx_attestation_test_host)
  endif()
endif()
