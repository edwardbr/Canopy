#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

CanopyGenerate(
  local_transport_config
  local_transport/local_transport_config.idl
  ${CMAKE_CURRENT_SOURCE_DIR}/interfaces
  ${CMAKE_BINARY_DIR}/generated
  ""
  yas_json
  dependencies rpc_types_idl
  include_paths ${CMAKE_CURRENT_SOURCE_DIR}/interfaces ${CANOPY_INTERFACE_SOURCE_DIR}
  install_dir ${GENERATED_INSTALL_DIR})

# Enclave version
if(CANOPY_BUILD_ENCLAVE)
  add_library(transport_local_enclave src/transport.cpp)

  target_compile_definitions(transport_local_enclave PRIVATE ${CANOPY_ENCLAVE_DEFINES})
  target_compile_options(transport_local_enclave PRIVATE ${CANOPY_ENCLAVE_COMPILE_OPTIONS} ${CANOPY_WARN_OK_ENCLAVE})

  target_include_directories(
    transport_local_enclave
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    PRIVATE ${CANOPY_ENCLAVE_LIBCXX_INCLUDES})

  target_link_libraries(transport_local_enclave PRIVATE yas_common rpc_types_idl_enclave)

  target_link_options(transport_local_enclave PRIVATE ${CANOPY_ENCLAVE_LINK_OPTIONS})

  set_property(TARGET transport_local_enclave PROPERTY COMPILE_PDB_NAME transport_local_enclave)

  if(CANOPY_ENABLE_CLANG_TIDY)
    set_target_properties(transport_local_enclave PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
  endif()
endif()

# Host version
add_library(transport_local src/transport.cpp src/factory.cpp)

target_compile_definitions(transport_local PRIVATE ${CANOPY_DEFINES})

target_include_directories(
  transport_local
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include>"
  PRIVATE ${CANOPY_INCLUDES})

target_link_libraries(transport_local PUBLIC transport_factory local_transport_config_idl PRIVATE yas_common rpc_types_idl ${CANOPY_LIBRARIES})

target_compile_options(transport_local PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(transport_local PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
set_property(TARGET transport_local PROPERTY COMPILE_PDB_NAME transport_local)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(transport_local PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()

add_library(transport_local_test INTERFACE)
target_include_directories(transport_local_test INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/tests>")
