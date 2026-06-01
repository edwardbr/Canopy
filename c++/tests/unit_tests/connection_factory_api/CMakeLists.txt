# Copyright (c) 2026 Edward Boggis-Rolfe All rights reserved.

cmake_minimum_required(VERSION 3.24)

add_executable(
  connection_factory_api_test
  public_config_api_test.cpp
  direct_stream_factory_api_test.cpp
  direct_transport_factory_api_test.cpp)

add_executable(connection_factory_public_header_smoke public_header_smoke.cpp)

target_compile_definitions(connection_factory_api_test PRIVATE ${CANOPY_DEFINES})
target_link_libraries(connection_factory_api_test PRIVATE GTest::gtest_main connection_factory rpc_log)
target_compile_definitions(connection_factory_public_header_smoke PRIVATE ${CANOPY_DEFINES})
target_link_libraries(connection_factory_public_header_smoke PRIVATE connection_factory rpc_log)

if(TARGET transport_local)
  target_link_libraries(connection_factory_api_test PRIVATE transport_local)
endif()

if(TARGET transport_sgx_blocking)
  target_link_libraries(connection_factory_api_test PRIVATE transport_sgx_blocking)
  target_compile_definitions(connection_factory_api_test PRIVATE CANOPY_HAS_SGX_BLOCKING_TRANSPORT_FACTORY)
endif()

if(TARGET transport_ipc_spsc)
  target_link_libraries(connection_factory_api_test PRIVATE transport_ipc_spsc)
  target_compile_definitions(connection_factory_api_test PRIVATE CANOPY_HAS_IPC_SPSC_TRANSPORT_FACTORY)
endif()

if(TARGET transport_shared_scheduler_dll)
  target_link_libraries(connection_factory_api_test PRIVATE transport_shared_scheduler_dll)
  target_compile_definitions(connection_factory_api_test PRIVATE CANOPY_HAS_SHARED_SCHEDULER_DLL_TRANSPORT_FACTORY)
endif()

if(TARGET transport_unshared_scheduler_dll)
  target_link_libraries(connection_factory_api_test PRIVATE transport_unshared_scheduler_dll)
  target_compile_definitions(connection_factory_api_test PRIVATE CANOPY_HAS_UNSHARED_SCHEDULER_DLL_TRANSPORT_FACTORY)
endif()

if(TARGET transport_sgx_coroutine)
  target_link_libraries(connection_factory_api_test PRIVATE transport_sgx_coroutine)
  target_compile_definitions(connection_factory_api_test PRIVATE CANOPY_HAS_SGX_COROUTINE_TRANSPORT_FACTORY)
endif()

if(CANOPY_BUILD_COROUTINE)
  if(TARGET streaming_tcp_coroutine)
    target_link_libraries(connection_factory_api_test PRIVATE streaming_tcp_coroutine)
  endif()
  if(TARGET streaming_spsc_queue)
    target_link_libraries(connection_factory_api_test PRIVATE streaming_spsc_queue)
  endif()
elseif(TARGET streaming_tcp_blocking)
  target_link_libraries(connection_factory_api_test PRIVATE streaming_tcp_blocking)
endif()

target_compile_options(connection_factory_api_test PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(connection_factory_api_test PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
set_property(TARGET connection_factory_api_test PROPERTY COMPILE_PDB_NAME connection_factory_api_test)
target_compile_options(connection_factory_public_header_smoke PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(connection_factory_public_header_smoke PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
set_property(TARGET connection_factory_public_header_smoke PROPERTY COMPILE_PDB_NAME connection_factory_public_header_smoke)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(connection_factory_api_test PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
  set_target_properties(connection_factory_public_header_smoke PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()

if(CANOPY_BUILD_TEST)
  include(GoogleTest)
  if(NOT CANOPY_DEBUG_ADDRESS)
    gtest_discover_tests(connection_factory_api_test)
  endif()
  add_test(NAME connection_factory_public_header_smoke COMMAND connection_factory_public_header_smoke)

  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/run_install_smoke.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/run_install_smoke.cmake"
    @ONLY)
  add_test(NAME connection_factory_install_smoke COMMAND ${CMAKE_COMMAND} -P
                                                   "${CMAKE_CURRENT_BINARY_DIR}/run_install_smoke.cmake")

  if(NOT CANOPY_BUILD_ENCLAVE)
    configure_file(
      "${CMAKE_CURRENT_SOURCE_DIR}/run_downstream_smoke.cmake.in"
      "${CMAKE_CURRENT_BINARY_DIR}/run_downstream_smoke.cmake"
      @ONLY)
    add_test(NAME connection_factory_downstream_smoke COMMAND ${CMAKE_COMMAND} -P
                                                         "${CMAKE_CURRENT_BINARY_DIR}/run_downstream_smoke.cmake")

    configure_file(
      "${CMAKE_CURRENT_SOURCE_DIR}/run_negative_typed_registration.cmake.in"
      "${CMAKE_CURRENT_BINARY_DIR}/run_negative_typed_registration.cmake"
      @ONLY)
    add_test(NAME connection_factory_negative_typed_registration COMMAND ${CMAKE_COMMAND} -P
                                                                    "${CMAKE_CURRENT_BINARY_DIR}/run_negative_typed_registration.cmake")
  endif()
endif()
