#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

add_library(canopy_dns_resolver STATIC src/resolver.cpp)

target_include_directories(
  canopy_dns_resolver
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include>"
  PRIVATE ${CANOPY_INCLUDES})

target_compile_definitions(canopy_dns_resolver PRIVATE ${CANOPY_DEFINES})
target_compile_options(canopy_dns_resolver PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(canopy_dns_resolver PRIVATE ${CANOPY_LINK_OPTIONS})
target_link_libraries(canopy_dns_resolver PUBLIC rpc PRIVATE canopy_cares)

set_property(TARGET canopy_dns_resolver PROPERTY COMPILE_PDB_NAME canopy_dns_resolver)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(canopy_dns_resolver PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()

if(CANOPY_BUILD_TEST)
  add_executable(dns_resolver_test tests/dns_resolver_test.cpp)
  target_compile_definitions(dns_resolver_test PRIVATE ${CANOPY_DEFINES})
  target_include_directories(dns_resolver_test PRIVATE ${CANOPY_INCLUDES})
  target_compile_options(dns_resolver_test PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
  target_link_options(dns_resolver_test PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
  target_link_libraries(dns_resolver_test PRIVATE canopy_dns_resolver GTest::gtest_main rpc_log)
  add_test(NAME dns_resolver_test COMMAND dns_resolver_test)
endif()

message(STATUS "canopy_dns_resolver configured")
