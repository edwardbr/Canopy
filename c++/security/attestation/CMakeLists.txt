#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

find_package(OpenSSL REQUIRED)

set(CANOPY_SECURITY_ATTESTATION_SOURCES
    src/aead.cpp
    src/backend_factory.cpp
    src/crypto_hash.cpp
    src/kdf.cpp
    src/backends/null/null_backend.cpp
    src/protected_rpc.cpp
    src/types.cpp
    src/zone_security_policy.cpp
    src/service.cpp)

# SGX-specific attestation backends were stripped out during the config topology
# refactor. Reintroduce them only as a dedicated backend module once that work
# has settled.
set(CANOPY_SECURITY_ATTESTATION_IDL_LIBS protected_rpc_protocol_idl)

if(CANOPY_ENABLE_DEVELOPMENT_ATTESTATION_BACKENDS)
  list(APPEND CANOPY_SECURITY_ATTESTATION_SOURCES src/backends/fake/fake_backend.cpp)
endif()

add_library(security_attestation STATIC ${CANOPY_SECURITY_ATTESTATION_SOURCES})
add_library(canopy::security_attestation ALIAS security_attestation)

target_include_directories(
  security_attestation
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include>"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" ${CANOPY_INCLUDES})
target_compile_definitions(security_attestation PRIVATE ${CANOPY_DEFINES})
target_compile_options(security_attestation PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(security_attestation PRIVATE ${CANOPY_LINK_OPTIONS})
target_link_libraries(
  security_attestation
  PUBLIC rpc
  PRIVATE ${CANOPY_SECURITY_ATTESTATION_IDL_LIBS} OpenSSL::Crypto)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(security_attestation PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()

if(CANOPY_SECURITY_ATTESTATION_BUILD_BACKEND_UNIT_TESTS)
  add_executable(attestation_service_test tests/attestation_service_test.cpp)
  target_include_directories(attestation_service_test PRIVATE ${CANOPY_INCLUDES})
  target_compile_definitions(attestation_service_test PRIVATE ${CANOPY_DEFINES})
  target_compile_options(attestation_service_test PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
  target_link_options(attestation_service_test PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
  target_link_libraries(attestation_service_test PRIVATE canopy::security_attestation route_attestation_protocol_idl
                                                         GTest::gtest_main rpc_log)
  add_test(NAME attestation_service_test COMMAND attestation_service_test)
  message(STATUS "attestation_service_test configured")
elseif(CANOPY_BUILD_TEST)
  message(STATUS "attestation_service_test not configured: development attestation backends are disabled")
endif()

message(STATUS "security_attestation configured")
