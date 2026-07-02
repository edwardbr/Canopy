#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

add_library(canopy_http_utils STATIC src/http.cpp)
target_include_directories(canopy_http_utils PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
                                                    "$<INSTALL_INTERFACE:include>")
target_compile_definitions(canopy_http_utils PRIVATE ${CANOPY_DEFINES})
target_compile_options(canopy_http_utils PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_PEDANTIC})
target_link_options(canopy_http_utils PRIVATE ${CANOPY_LINK_OPTIONS})

set_property(TARGET canopy_http_utils PROPERTY COMPILE_PDB_NAME canopy_http_utils)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(canopy_http_utils PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()

add_library(canopy::http_utils ALIAS canopy_http_utils)

if(CANOPY_BUILD_ENCLAVE)
  add_library(canopy_http_utils_enclave STATIC src/http.cpp)
  target_include_directories(
    canopy_http_utils_enclave
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include>"
    PRIVATE ${CANOPY_ENCLAVE_LIBCXX_INCLUDES})
  target_compile_definitions(canopy_http_utils_enclave PRIVATE ${CANOPY_ENCLAVE_DEFINES})
  target_compile_options(canopy_http_utils_enclave PRIVATE ${CANOPY_ENCLAVE_COMPILE_OPTIONS} ${CANOPY_WARN_PEDANTIC_ENCLAVE})
  target_link_options(canopy_http_utils_enclave PRIVATE ${CANOPY_ENCLAVE_LINK_OPTIONS})

  set_property(TARGET canopy_http_utils_enclave PROPERTY COMPILE_PDB_NAME canopy_http_utils_enclave)

  if(CANOPY_ENABLE_CLANG_TIDY)
    set_target_properties(canopy_http_utils_enclave PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
  endif()

  add_library(canopy::http_utils_enclave ALIAS canopy_http_utils_enclave)
endif()

message(STATUS "canopy_http_utils configured")
