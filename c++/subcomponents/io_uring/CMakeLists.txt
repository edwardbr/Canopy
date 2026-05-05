#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

add_subdirectory(interfaces)

set(CANOPY_IO_URING_ENCLAVE_SOURCES
    src/direct_descriptor.cpp
    src/controller_core.cpp
    src/controller_buffers.cpp
    src/controller_noop.cpp
    src/controller_submission.cpp
    src/controller_tcp.cpp)

if(CANOPY_BUILD_ENCLAVE)
  add_library(canopy_io_uring_enclave STATIC ${CANOPY_IO_URING_ENCLAVE_SOURCES})

  target_include_directories(
    canopy_io_uring_enclave
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
           "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated/include>"
           "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated/src>"
           "$<INSTALL_INTERFACE:include>"
    PRIVATE ${CANOPY_ENCLAVE_LIBCXX_INCLUDES})

  target_compile_definitions(canopy_io_uring_enclave PRIVATE ${CANOPY_ENCLAVE_DEFINES})
  target_compile_options(canopy_io_uring_enclave PRIVATE ${CANOPY_ENCLAVE_COMPILE_OPTIONS} ${CANOPY_WARN_OK_ENCLAVE})
  target_link_options(canopy_io_uring_enclave PRIVATE ${CANOPY_ENCLAVE_LINK_OPTIONS})

  target_link_libraries(canopy_io_uring_enclave PUBLIC rpc::rpc_enclave io_uring_idl_enclave libcoro_enclave)

  set_property(TARGET canopy_io_uring_enclave PROPERTY COMPILE_PDB_NAME canopy_io_uring_enclave)

  if(CANOPY_ENABLE_CLANG_TIDY)
    set_target_properties(canopy_io_uring_enclave PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
  endif()
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_library(canopy_io_uring STATIC ${CANOPY_IO_URING_ENCLAVE_SOURCES})
  target_include_directories(
    canopy_io_uring
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
           "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated/include>"
           "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated/src>"
           "$<INSTALL_INTERFACE:include>"
    PRIVATE ${CANOPY_INCLUDES})
  target_compile_definitions(canopy_io_uring PRIVATE ${CANOPY_DEFINES})
  target_compile_options(canopy_io_uring PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
  target_link_options(canopy_io_uring PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
  target_link_libraries(canopy_io_uring PUBLIC rpc::rpc io_uring_idl PRIVATE libcoro ${CANOPY_LIBRARIES})
  message(STATUS "canopy_io_uring configured without host io_uring control on non-Linux platform")
  return()
endif()

add_library(canopy_io_uring STATIC host_control/host_control.cpp ${CANOPY_IO_URING_ENCLAVE_SOURCES})

target_include_directories(
  canopy_io_uring
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
         "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated/include>"
         "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated/src>"
         "$<INSTALL_INTERFACE:include>"
  PRIVATE ${CANOPY_INCLUDES})

target_compile_definitions(canopy_io_uring PRIVATE ${CANOPY_DEFINES})
target_compile_options(canopy_io_uring PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(canopy_io_uring PRIVATE ${CANOPY_LINK_EXE_OPTIONS})

target_link_libraries(
  canopy_io_uring
  PUBLIC rpc::rpc io_uring_idl
  PRIVATE libcoro uring ${CANOPY_LIBRARIES} ${CANOPY_FMT_LIB})

set_property(TARGET canopy_io_uring PROPERTY COMPILE_PDB_NAME canopy_io_uring)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(canopy_io_uring PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()

if(CANOPY_BUILD_TEST
   AND CANOPY_BUILD_COROUTINE
   AND TARGET transport_streaming
   AND TARGET streaming_tls
   AND TARGET streaming_websocket)
  add_executable(io_uring_stream_composition_fit tests/stream_composition_fit.cpp)
  target_include_directories(io_uring_stream_composition_fit PRIVATE ${CANOPY_INCLUDES})
  target_compile_definitions(io_uring_stream_composition_fit PRIVATE ${CANOPY_DEFINES})
  target_compile_options(io_uring_stream_composition_fit PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
  target_link_options(io_uring_stream_composition_fit PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
  target_link_libraries(
    io_uring_stream_composition_fit
    PRIVATE canopy_io_uring
            streaming_tls
            streaming_websocket
            transport_streaming
            rpc_log)
  set_property(TARGET io_uring_stream_composition_fit PROPERTY COMPILE_PDB_NAME io_uring_stream_composition_fit)
  message(STATUS "io_uring_stream_composition_fit configured")
endif()

if(CANOPY_BUILD_TEST
   AND CANOPY_BUILD_ENCLAVE
   AND TARGET canopy_io_uring_enclave
   AND TARGET transport_streaming_enclave
   AND TARGET streaming_tls_enclave
   AND TARGET streaming_websocket_enclave)
  add_library(io_uring_stream_composition_fit_enclave STATIC tests/stream_composition_fit.cpp)
  target_include_directories(io_uring_stream_composition_fit_enclave PRIVATE ${CANOPY_ENCLAVE_LIBCXX_INCLUDES})
  target_compile_definitions(io_uring_stream_composition_fit_enclave PRIVATE ${CANOPY_ENCLAVE_DEFINES})
  target_compile_options(io_uring_stream_composition_fit_enclave PRIVATE ${CANOPY_ENCLAVE_COMPILE_OPTIONS}
                                                                         ${CANOPY_WARN_OK_ENCLAVE})
  target_link_options(io_uring_stream_composition_fit_enclave PRIVATE ${CANOPY_ENCLAVE_LINK_OPTIONS})
  target_link_libraries(
    io_uring_stream_composition_fit_enclave
    PRIVATE canopy_io_uring_enclave
            streaming_tls_enclave
            streaming_websocket_enclave
            transport_streaming_enclave)
  set_property(TARGET io_uring_stream_composition_fit_enclave PROPERTY COMPILE_PDB_NAME
                                                                 io_uring_stream_composition_fit_enclave)
  message(STATUS "io_uring_stream_composition_fit_enclave configured")
endif()

message(STATUS "canopy_io_uring configured")
