#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

add_library(security_attestation STATIC src/fake_backend.cpp)
add_library(canopy::security_attestation ALIAS security_attestation)

target_include_directories(
  security_attestation
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include>"
  PRIVATE ${CANOPY_INCLUDES})
target_compile_definitions(security_attestation PRIVATE ${CANOPY_DEFINES})
target_compile_options(security_attestation PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(security_attestation PRIVATE ${CANOPY_LINK_OPTIONS})
target_link_libraries(security_attestation PUBLIC rpc)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(security_attestation PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()

message(STATUS "security_attestation configured")
