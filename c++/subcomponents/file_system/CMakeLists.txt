#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

CanopyGenerate(
  file_system
  file_system/file_system.idl
  ${CMAKE_CURRENT_SOURCE_DIR}/interfaces
  ${CMAKE_BINARY_DIR}/generated
  ""
  yas_binary
  yas_compressed_binary
  yas_json
  nanopb
  include_paths ${CMAKE_CURRENT_SOURCE_DIR}/interfaces)

add_library(canopy_file_system STATIC src/file_system_manager.cpp)

target_include_directories(
  canopy_file_system
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
         "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated/include>" "$<INSTALL_INTERFACE:include>"
  PRIVATE ${CANOPY_INCLUDES})

target_compile_definitions(canopy_file_system PRIVATE ${CANOPY_DEFINES})
target_compile_options(canopy_file_system PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(canopy_file_system PRIVATE ${CANOPY_LINK_EXE_OPTIONS})

if(CANOPY_BUILD_COROUTINE)
  target_link_libraries(canopy_file_system PUBLIC file_system_idl canopy_io_uring rpc::rpc)
else()
  # Blocking-mode posix backend: no io_uring controller.
  target_link_libraries(canopy_file_system PUBLIC file_system_idl rpc::rpc)
endif()

set_property(TARGET canopy_file_system PROPERTY COMPILE_PDB_NAME canopy_file_system)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(canopy_file_system PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()

if(CANOPY_BUILD_ENCLAVE AND CANOPY_BUILD_COROUTINE AND TARGET canopy_io_uring_enclave)
  add_library(canopy_file_system_enclave STATIC src/file_system_manager.cpp)

  target_include_directories(
    canopy_file_system_enclave
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
           "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated/include>" "$<INSTALL_INTERFACE:include>"
    PRIVATE ${CANOPY_ENCLAVE_LIBCXX_INCLUDES})

  target_compile_definitions(canopy_file_system_enclave PRIVATE ${CANOPY_ENCLAVE_DEFINES})
  target_compile_options(canopy_file_system_enclave PRIVATE ${CANOPY_ENCLAVE_COMPILE_OPTIONS} ${CANOPY_WARN_OK_ENCLAVE})
  target_link_options(canopy_file_system_enclave PRIVATE ${CANOPY_ENCLAVE_LINK_OPTIONS})

  target_link_libraries(canopy_file_system_enclave PUBLIC file_system_idl_enclave canopy_io_uring_enclave
                                                          rpc::rpc_enclave)

  set_property(TARGET canopy_file_system_enclave PROPERTY COMPILE_PDB_NAME canopy_file_system_enclave)

  if(CANOPY_ENABLE_CLANG_TIDY)
    set_target_properties(canopy_file_system_enclave PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
  endif()
endif()

message(STATUS "canopy_file_system configured")
