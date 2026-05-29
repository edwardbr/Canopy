#
# Copyright (c) 2026 Edward Boggis-Rolfe All rights reserved.
#

cmake_minimum_required(VERSION 3.24)

set(STREAMING_IO_URING_SOURCES src/acceptor.cpp src/connector.cpp src/stream.cpp)

add_library(streaming_io_uring STATIC ${STREAMING_IO_URING_SOURCES})
target_include_directories(
  streaming_io_uring
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include>"
  PRIVATE ${CANOPY_INCLUDES})
target_compile_definitions(streaming_io_uring PRIVATE ${CANOPY_DEFINES})
target_compile_options(streaming_io_uring PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(streaming_io_uring PRIVATE ${CANOPY_LINK_OPTIONS})
target_link_libraries(
  streaming_io_uring
  PUBLIC streaming canopy_io_uring io_uring_config_idl
  PRIVATE rpc libcoro)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(streaming_io_uring PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()

if(CANOPY_BUILD_ENCLAVE)
  add_library(streaming_io_uring_enclave STATIC ${STREAMING_IO_URING_SOURCES})
  target_include_directories(
    streaming_io_uring_enclave
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include>"
    PRIVATE ${CANOPY_ENCLAVE_LIBCXX_INCLUDES})
  target_compile_definitions(streaming_io_uring_enclave PRIVATE ${CANOPY_ENCLAVE_DEFINES})
  target_compile_options(streaming_io_uring_enclave PRIVATE ${CANOPY_ENCLAVE_COMPILE_OPTIONS} ${CANOPY_WARN_OK_ENCLAVE})
  target_link_options(streaming_io_uring_enclave PRIVATE ${CANOPY_ENCLAVE_LINK_OPTIONS})
  target_link_libraries(
    streaming_io_uring_enclave
    PUBLIC streaming_enclave canopy_io_uring_enclave io_uring_config_idl_enclave
    PRIVATE rpc_enclave)

  if(CANOPY_ENABLE_CLANG_TIDY)
    set_target_properties(streaming_io_uring_enclave PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
  endif()
endif()

if(CANOPY_BUILD_TEST
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
    PRIVATE streaming_io_uring
            streaming_tls
            streaming_websocket
            transport_streaming
            rpc_log)
  set_property(TARGET io_uring_stream_composition_fit PROPERTY COMPILE_PDB_NAME io_uring_stream_composition_fit)
  message(STATUS "io_uring_stream_composition_fit configured")
endif()

if(CANOPY_BUILD_TEST
   AND CANOPY_BUILD_ENCLAVE
   AND TARGET streaming_io_uring_enclave
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
    io_uring_stream_composition_fit_enclave PRIVATE streaming_io_uring_enclave streaming_tls_enclave
                                                    streaming_websocket_enclave transport_streaming_enclave)
  set_property(TARGET io_uring_stream_composition_fit_enclave PROPERTY COMPILE_PDB_NAME
                                                                       io_uring_stream_composition_fit_enclave)
  message(STATUS "io_uring_stream_composition_fit_enclave configured")
endif()
