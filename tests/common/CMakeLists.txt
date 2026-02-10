#[[
   Copyright (c) 2024 Edward Boggis-Rolfe
   All rights reserved.
]]
cmake_minimum_required(VERSION 3.24)

message("common_enclave")

# ######################################################################################################################
# common_enclave

if(${CANOPY_BUILD_ENCLAVE})
  add_library(common_enclave STATIC src/host_service_proxy.cpp)

  target_compile_definitions(common_enclave PRIVATE ${CANOPY_ENCLAVE_DEFINES})
  target_include_directories(
    common_enclave
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include/marshalled_tests>"
    PRIVATE ${CANOPY_ENCLAVE_LIBCXX_INCLUDES})

  target_compile_options(common_enclave PRIVATE ${CANOPY_ENCLAVE_COMPILE_OPTIONS} ${CANOPY_WARN_PEDANTIC})
  target_link_options(common_enclave PRIVATE ${CANOPY_ENCLAVE_LINK_OPTIONS})

  target_link_libraries(
    common_enclave
    PRIVATE rpc_enclave
            marshal_test_edl_enclave
            ${CANOPY_TELEMETRY_ENCLAVE_EDL}
            ${CANOPY_ENCLAVE_FMT_LIB}
            example_idl_enclave
            yas_common)

  set_property(TARGET common_enclave PROPERTY COMPILE_PDB_NAME common_enclave)

  # ####################################################################################################################

endif()

set(SOURCES ${COROUTINE_SERVICE_PROXIES} src/foo_impl.cpp src/tests.cpp)
if(CANOPY_BUILD_ENCLAVE)
  list(APPEND SOURCES src/enclave_service_proxy.cpp)
  set(ADDITIONAL_TARGET marshal_test_edl rpc_telemetry_edl_header)
endif()

add_library(common_tests STATIC ${SOURCES})

target_compile_definitions(common_tests PRIVATE ${CANOPY_DEFINES})
target_include_directories(
  common_tests
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include/marshalled_tests>"
  PRIVATE ${CANOPY_INCLUDES})

target_compile_options(common_tests PRIVATE ${CANOPY_COMPILE_OPTIONS})
target_link_options(common_tests PRIVATE ${CANOPY_LINK_OPTIONS})

if(${CANOPY_BUILD_COROUTINE})
  set(CORO_IDL tcp_idl spsc_idl)
else()
  set(CORO_IDL)
endif()

target_link_libraries(
  common_tests
  PRIVATE rpc::rpc
          ${ADDITIONAL_TARGET}
          example_idl
          example_import_idl
          example_shared_idl
          transport_local
          ${CANOPY_FMT_LIB}
          ${CORO_IDL}
          GTest::gtest_main
          GTest::gmock_main
          yas_common)

set_property(TARGET common_tests PROPERTY COMPILE_PDB_NAME common_tests)
