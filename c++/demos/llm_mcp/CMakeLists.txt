#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)
project(
  llm_mcp_demo
  VERSION 1.0.0
  LANGUAGES CXX)

add_subdirectory(idl)

add_library(llm_mcp_logic src/logic.cpp)
target_compile_definitions(llm_mcp_logic PRIVATE ${CANOPY_DEFINES})
target_include_directories(
  llm_mcp_logic
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  PRIVATE ${CANOPY_INCLUDES})
target_link_libraries(llm_mcp_logic PUBLIC llm_mcp_idl rpc ${CANOPY_LIBRARIES} rpc_log)
target_compile_options(llm_mcp_logic PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(llm_mcp_logic PRIVATE ${CANOPY_LINK_OPTIONS})
set_property(TARGET llm_mcp_logic PROPERTY COMPILE_PDB_NAME llm_mcp_logic)

if(TARGET llama)
  target_sources(llm_mcp_logic PRIVATE src/llama_cpp_backend.cpp)
  target_compile_definitions(llm_mcp_logic PRIVATE CANOPY_LLM_MCP_HAS_LLAMA_CPP)
  target_link_libraries(llm_mcp_logic PRIVATE llama)
endif()

add_executable(llm_mcp_harness harness/main.cpp)
target_compile_definitions(llm_mcp_harness PRIVATE ${CANOPY_DEFINES})
target_include_directories(
  llm_mcp_harness
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  PRIVATE ${CANOPY_INCLUDES})
target_link_libraries(
  llm_mcp_harness
  PUBLIC llm_mcp_logic
         llm_mcp_harness_idl
         demo_calculator_rpc_object
         demo_text_tools_rpc_object
         calculator_idl
         text_tools_idl
         rpc
         ${CANOPY_LIBRARIES}
         rpc_log)
target_compile_options(llm_mcp_harness PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(llm_mcp_harness PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
set_property(TARGET llm_mcp_harness PROPERTY COMPILE_PDB_NAME llm_mcp_harness)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(llm_mcp_logic llm_mcp_harness PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()
