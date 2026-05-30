#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

if(CANOPY_BUILD_COROUTINE AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(file_system_test_host file_system_test_host.cpp)

  target_compile_definitions(file_system_test_host PRIVATE ${CANOPY_DEFINES})
  if(CANOPY_BUILD_ENCLAVE)
    target_compile_definitions(
      file_system_test_host PRIVATE CANOPY_FILE_SYSTEM_TEST_ENCLAVE_PATH="${file_system_test_enclave_sign_OUTPUT_NAME}")
  endif()

  target_include_directories(
    file_system_test_host
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated/include>"
           "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated/src>"
    PRIVATE ${CANOPY_INCLUDES})

  target_link_libraries(
    file_system_test_host
    PUBLIC file_system_idl
           canopy_file_system
           canopy_io_uring
           rpc::rpc
           GTest::gtest_main
           fmt::fmt-header-only
           spdlog::spdlog
           ${CANOPY_FMT_LIB}
           ${CANOPY_LIBRARIES}
           rpc_log)

  if(CANOPY_BUILD_ENCLAVE)
    target_link_libraries(file_system_test_host PUBLIC transport_sgx_coroutine transport_sgx_coroutine_test
                                                       sgx_coroutine_transport_edl ${CANOPY_SGX_HOST_LIBRARIES})
    target_link_directories(file_system_test_host PUBLIC ${CANOPY_SGX_HOST_LIBRARY_DIRS})
    add_dependencies(file_system_test_host file_system_test_enclave)
  endif()

  target_compile_options(file_system_test_host PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
  target_link_options(file_system_test_host PRIVATE ${CANOPY_LINK_EXE_OPTIONS} ${CANOPY_SGX_HOST_LINK_OPTIONS})
  set_property(TARGET file_system_test_host PROPERTY COMPILE_PDB_NAME file_system_test_host)

  if(CANOPY_ENABLE_CLANG_TIDY)
    set_target_properties(file_system_test_host PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
  endif()

  include(CTest)
  if(NOT CANOPY_DEBUG_ADDRESS)
    gtest_discover_tests(file_system_test_host)
  endif()
endif()
