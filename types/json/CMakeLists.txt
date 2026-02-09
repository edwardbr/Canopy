# Copyright (c) 2026 Edward Boggis-Rolfe All rights reserved.

cmake_minimum_required(VERSION 3.12)

add_subdirectory(interface)

# add_executable(rpc_json json.cpp)

# target_compile_definitions(rpc_json PRIVATE ${CANOPY_DEFINES})

# target_include_directories( rpc_json PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../../include>" PRIVATE
# ${CANOPY_INCLUDES})

# target_link_libraries(rpc_json PUBLIC rpc yas_common ${CANOPY_LIBRARIES}) target_compile_options(rpc_json PRIVATE
# ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_PEDANTIC}) target_link_options(rpc_json PRIVATE ${CANOPY_LINK_EXE_OPTIONS})

# set_property(TARGET rpc_json PROPERTY COMPILE_PDB_NAME rpc_json)

# if(CANOPY_ENABLE_CLANG_TIDY) set_target_properties(rpc_json PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}") endif()
