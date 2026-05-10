#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

set(CANOPY_IO_URING_ENCLAVE_SOURCES
    src/direct_descriptor.cpp
    src/controller_core.cpp
    src/controller_buffers.cpp
    src/controller_noop.cpp
    src/controller_submission.cpp
    src/controller_tcp.cpp
    src/io_uring_scheduler.cpp)

if(CANOPY_BUILD_ENCLAVE)
  add_library(canopy_io_uring_enclave STATIC ${CANOPY_IO_URING_ENCLAVE_SOURCES})

  target_include_directories(
    canopy_io_uring_enclave
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include>"
    PRIVATE ${CANOPY_ENCLAVE_LIBCXX_INCLUDES})

  target_compile_definitions(canopy_io_uring_enclave PRIVATE ${CANOPY_ENCLAVE_DEFINES})
  target_compile_options(canopy_io_uring_enclave PRIVATE ${CANOPY_ENCLAVE_COMPILE_OPTIONS} ${CANOPY_WARN_OK_ENCLAVE})
  target_link_options(canopy_io_uring_enclave PRIVATE ${CANOPY_ENCLAVE_LINK_OPTIONS})

  target_link_libraries(canopy_io_uring_enclave PUBLIC rpc::rpc_enclave libcoro_enclave)

  set_property(TARGET canopy_io_uring_enclave PROPERTY COMPILE_PDB_NAME canopy_io_uring_enclave)

  if(CANOPY_ENABLE_CLANG_TIDY)
    set_target_properties(canopy_io_uring_enclave PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
  endif()
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_library(canopy_io_uring STATIC ${CANOPY_IO_URING_ENCLAVE_SOURCES})
  target_include_directories(canopy_io_uring PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
                                                    "$<INSTALL_INTERFACE:include>")
  target_compile_definitions(canopy_io_uring PRIVATE ${CANOPY_DEFINES})
  target_compile_options(canopy_io_uring PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
  target_link_options(canopy_io_uring PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
  target_link_libraries(
    canopy_io_uring
    PUBLIC rpc::rpc
    PRIVATE libcoro ${CANOPY_LIBRARIES})
  message(STATUS "canopy_io_uring configured without host io_uring control on non-Linux platform")
  return()
endif()

add_library(canopy_io_uring STATIC host_control/host_control.cpp host_control/host_io_uring.cpp
                                   host_control/linux_io_uring_handle.cpp ${CANOPY_IO_URING_ENCLAVE_SOURCES})

target_include_directories(canopy_io_uring PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
                                                  "$<INSTALL_INTERFACE:include>")

target_compile_definitions(canopy_io_uring PRIVATE ${CANOPY_DEFINES})
target_compile_options(canopy_io_uring PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(canopy_io_uring PRIVATE ${CANOPY_LINK_EXE_OPTIONS})

target_link_libraries(
  canopy_io_uring
  PUBLIC rpc::rpc
  PRIVATE libcoro uring ${CANOPY_LIBRARIES} ${CANOPY_FMT_LIB})

set_property(TARGET canopy_io_uring PROPERTY COMPILE_PDB_NAME canopy_io_uring)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(canopy_io_uring PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()

if(CANOPY_BUILD_TEST AND CANOPY_BUILD_COROUTINE)
  add_executable(io_uring_host_scheduler_test tests/host_io_uring_scheduler_test.cpp)
  target_include_directories(io_uring_host_scheduler_test PRIVATE ${CANOPY_INCLUDES})
  target_compile_definitions(io_uring_host_scheduler_test PRIVATE ${CANOPY_DEFINES})
  target_compile_options(io_uring_host_scheduler_test PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
  target_link_options(io_uring_host_scheduler_test PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
  target_link_libraries(io_uring_host_scheduler_test PRIVATE canopy_io_uring GTest::gtest_main rpc_log)
  add_test(NAME io_uring_host_scheduler_test COMMAND io_uring_host_scheduler_test)
  set_property(TARGET io_uring_host_scheduler_test PROPERTY COMPILE_PDB_NAME io_uring_host_scheduler_test)
  message(STATUS "io_uring_host_scheduler_test configured")
endif()

message(STATUS "canopy_io_uring configured")
