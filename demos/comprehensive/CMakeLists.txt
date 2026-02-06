#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]
#
# Comprehensive Demos CMakeLists.txt Demonstrates all major Canopy features
#

cmake_minimum_required(VERSION 3.24)
project(
  comprehensive_demos
  VERSION 1.0.0
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(idl)

# Find all demo source files
file(GLOB_RECURSE DEMO_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*/*.cpp)

# Create executables for each demo category Note: These require the generated demo.h header which is created by
# rpc_generator

# Transport demos (require Coroutine_Debug for TCP and SPSC)
add_executable(local_transport_demo src/transport/local_transport_demo.cpp)
target_compile_definitions(local_transport_demo PRIVATE ${CANOPY_DEFINES})
target_include_directories(
  local_transport_demo
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  PRIVATE ${CANOPY_INCLUDES})
target_link_libraries(local_transport_demo PUBLIC comprehensive_idl transport_local rpc ${CANOPY_LIBRARIES})
target_compile_options(local_transport_demo PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(local_transport_demo PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
set_property(TARGET local_transport_demo PROPERTY COMPILE_PDB_NAME local_transport_demo)

if(CANOPY_BUILD_COROUTINE)
  add_executable(tcp_transport_demo src/transport/tcp_transport_demo.cpp)
  target_compile_definitions(tcp_transport_demo PRIVATE ${CANOPY_DEFINES})
  target_include_directories(
    tcp_transport_demo
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    PRIVATE ${CANOPY_INCLUDES} transport_tcp)
  target_link_libraries(
    tcp_transport_demo
    PUBLIC comprehensive_idl
           transport_local
           transport_tcp
           rpc
           ${CANOPY_LIBRARIES})
  target_compile_options(tcp_transport_demo PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
  target_link_options(tcp_transport_demo PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
  set_property(TARGET tcp_transport_demo PROPERTY COMPILE_PDB_NAME tcp_transport_demo)
endif()

if(CANOPY_BUILD_COROUTINE)
  add_executable(spsc_transport_demo src/transport/spsc_transport_demo.cpp)
  target_compile_definitions(spsc_transport_demo PRIVATE ${CANOPY_DEFINES})
  target_include_directories(
    spsc_transport_demo
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    PRIVATE ${CANOPY_INCLUDES} transport_spsc)
  target_link_libraries(
    spsc_transport_demo
    PUBLIC comprehensive_idl
           transport_local
           transport_spsc
           rpc
           ${CANOPY_LIBRARIES})
  target_compile_options(spsc_transport_demo PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
  target_link_options(spsc_transport_demo PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
  set_property(TARGET spsc_transport_demo PROPERTY COMPILE_PDB_NAME spsc_transport_demo)
endif()

if(CANOPY_BUILD_COROUTINE)
  add_executable(benchmark src/transport/benchmark.cpp)
  target_compile_definitions(benchmark PRIVATE ${CANOPY_DEFINES})
  target_include_directories(
    benchmark
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    PRIVATE ${CANOPY_INCLUDES} transport_tcp transport_spsc)
  target_link_libraries(
    benchmark
    PUBLIC comprehensive_idl
           transport_local
           transport_tcp
           transport_spsc
           rpc
           ${CANOPY_LIBRARIES})
  target_compile_options(benchmark PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
  target_link_options(benchmark PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
  set_property(TARGET benchmark PROPERTY COMPILE_PDB_NAME benchmark)
endif()

# Serialization demo
add_executable(serialisation_demo src/serialisation/serialisation_demo.cpp)
target_compile_definitions(serialisation_demo PRIVATE ${CANOPY_DEFINES})
target_include_directories(
  serialisation_demo
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  PRIVATE ${CANOPY_INCLUDES})
target_link_libraries(serialisation_demo PUBLIC comprehensive_idl rpc transport_local ${CANOPY_LIBRARIES})
target_compile_options(serialisation_demo PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(serialisation_demo PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
set_property(TARGET serialisation_demo PROPERTY COMPILE_PDB_NAME serialisation_demo)

# Pointer demos
add_executable(shared_ptr_demo src/pointers/shared_ptr_demo.cpp)
target_compile_definitions(shared_ptr_demo PRIVATE ${CANOPY_DEFINES})
target_include_directories(
  shared_ptr_demo
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  PRIVATE ${CANOPY_INCLUDES})
target_link_libraries(shared_ptr_demo PUBLIC comprehensive_idl rpc transport_local ${CANOPY_LIBRARIES})
target_compile_options(shared_ptr_demo PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(shared_ptr_demo PRIVATE ${CANOPY_LINK_EXE_OPTIONS})

add_executable(optimistic_ptr_demo src/pointers/optimistic_ptr_demo.cpp)
target_compile_definitions(optimistic_ptr_demo PRIVATE ${CANOPY_DEFINES})
target_include_directories(
  optimistic_ptr_demo
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  PRIVATE ${CANOPY_INCLUDES})
target_link_libraries(optimistic_ptr_demo PUBLIC comprehensive_idl rpc transport_local ${CANOPY_LIBRARIES})
target_compile_options(optimistic_ptr_demo PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(optimistic_ptr_demo PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
set_property(TARGET optimistic_ptr_demo PROPERTY COMPILE_PDB_NAME optimistic_ptr_demo)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(
    local_transport_demo
    tcp_transport_demo
    spsc_transport_demo
    benchmark
    serialisation_demo
    shared_ptr_demo
    optimistic_ptr_demo
    PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()

message(STATUS "Comprehensive demos configured")
message(STATUS "Demo sources: ${DEMO_SOURCES}")
message(STATUS "")
message(STATUS "To build these demos:")
message(STATUS "1. Ensure Canopy is built with: cmake --preset Coroutine_Debug")
message(STATUS "2. Generate IDL code with: cmake --build build --target demo_idl")
message(STATUS "3. Build with: cmake --build build --target <demo_name>")
message(STATUS "")
message(STATUS "Available demos:")
message(STATUS "  - local_transport_demo (in-process communication)")
message(STATUS "  - tcp_transport_demo (network transport, requires coroutines)")
message(STATUS "  - spsc_transport_demo (IPC, requires coroutines)")
message(STATUS "  - benchmark (transport/encoding/blob matrix, requires coroutines)")
message(STATUS "  - serialisation_demo (serialization formats)")
message(STATUS "  - shared_ptr_demo (rpc::shared_ptr reference counting)")
message(STATUS "  - optimistic_ptr_demo (rpc::optimistic_ptr lifecycle)")
