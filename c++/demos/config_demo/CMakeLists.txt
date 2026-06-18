#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)
project(
  config_demo
  VERSION 1.0.0
  LANGUAGES CXX)

add_subdirectory(idl)
include("${CANOPY_CPP_SOURCE_DIR}/connection_factory/cmake/CanopyConnectionConfigSchema.cmake")

set(CONFIG_DEMO_CERT_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/certs")
set(CONFIG_DEMO_CERT_DIR "${CMAKE_CURRENT_BINARY_DIR}/certs")
set(CONFIG_DEMO_SAMPLE_DIR "${CMAKE_CURRENT_BINARY_DIR}/samples")
set(CONFIG_DEMO_SCHEMA_DIR "${CMAKE_CURRENT_BINARY_DIR}/schemas")
set(CONFIG_DEMO_SOURCE_SCHEMA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/schemas")

file(MAKE_DIRECTORY "${CONFIG_DEMO_CERT_DIR}" "${CONFIG_DEMO_SAMPLE_DIR}" "${CONFIG_DEMO_SCHEMA_DIR}")
file(COPY "${CONFIG_DEMO_CERT_SOURCE_DIR}/" DESTINATION "${CONFIG_DEMO_CERT_DIR}")
file(
  COPY "${CMAKE_CURRENT_SOURCE_DIR}/samples/"
  DESTINATION "${CONFIG_DEMO_SAMPLE_DIR}"
  FILES_MATCHING
  PATTERN "*.json")

add_executable(config_demo src/config_demo.cpp src/config_processing.cpp src/rpc_demo.cpp)

target_compile_definitions(config_demo PRIVATE ${CANOPY_DEFINES} CONFIG_DEMO_SAMPLE_DIR="${CONFIG_DEMO_SAMPLE_DIR}")

target_include_directories(
  config_demo
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  PRIVATE ${CANOPY_INCLUDES})

target_link_libraries(
  config_demo
  PUBLIC config_demo_idl
         config_demo_config_idl
         demo_calculator_rpc_object
         calculator_idl
         connection_factory
         rpc::modules
         ${CANOPY_LIBRARIES}
         rpc_log)

target_compile_options(config_demo PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(config_demo PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
set_property(TARGET config_demo PROPERTY COMPILE_PDB_NAME config_demo)

canopygenerateconnectionconfigschema(
  config_demo_config_idl
  "${CONFIG_DEMO_SCHEMA_DIR}"
  TARGET
  config_demo_schema
  ROOT_DEFINITION
  config_demo_v1_demo_settings
  OUTPUT_SCHEMA
  config_demo_config.schema.json)
canopygenerateconnectionconfigschema(
  config_demo_config_idl
  "${CONFIG_DEMO_SOURCE_SCHEMA_DIR}"
  TARGET
  config_demo_source_schema
  ROOT_DEFINITION
  config_demo_v1_demo_settings
  OUTPUT_SCHEMA
  config_demo_config.schema.json)
add_dependencies(config_demo config_demo_schema config_demo_source_schema)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(config_demo PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()

message(STATUS "config_demo configured")
message(STATUS "  - executable: config_demo")
message(STATUS "  - samples: ${CONFIG_DEMO_SAMPLE_DIR}")
message(STATUS "  - schemas: ${CONFIG_DEMO_SCHEMA_DIR}")
