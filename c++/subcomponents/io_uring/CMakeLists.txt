#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

set(CANOPY_IO_URING_ENCLAVE_SOURCES
    src/direct_descriptor.cpp
    src/controller_core.cpp
    src/controller_buffers.cpp
    src/controller_file.cpp
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

  target_link_libraries(canopy_io_uring_enclave PUBLIC rpc::rpc_enclave libcoro_enclave io_uring_config_idl_enclave)

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
    PUBLIC rpc::rpc io_uring_config_idl
    PRIVATE libcoro ${CANOPY_LIBRARIES})
  message(STATUS "canopy_io_uring configured without Linux io_uring control on non-Linux platform")
  return()
endif()

find_path(CANOPY_LIBURING_INCLUDE_DIR NAMES liburing.h)
find_library(CANOPY_LIBURING_LIBRARY NAMES uring)

set(CANOPY_LIBURING_BOOTSTRAP_TARGET)

if(CANOPY_LIBURING_INCLUDE_DIR AND CANOPY_LIBURING_LIBRARY)
  add_library(canopy_liburing UNKNOWN IMPORTED)
  set_target_properties(canopy_liburing PROPERTIES IMPORTED_LOCATION "${CANOPY_LIBURING_LIBRARY}"
                                                   INTERFACE_INCLUDE_DIRECTORIES "${CANOPY_LIBURING_INCLUDE_DIR}")
  message(STATUS "Using system liburing: ${CANOPY_LIBURING_LIBRARY}")
else()
  include(ExternalProject)
  find_program(CANOPY_MAKE_EXECUTABLE NAMES make gmake REQUIRED)

  set(CANOPY_LIBURING_VERSION "2.5")
  set(CANOPY_LIBURING_PREFIX "${CMAKE_BINARY_DIR}/liburing_bootstrap")
  set(CANOPY_LIBURING_INSTALL_DIR "${CANOPY_LIBURING_PREFIX}/install")
  set(CANOPY_LIBURING_BOOTSTRAP_INCLUDE_DIR "${CANOPY_LIBURING_INSTALL_DIR}/include")
  set(CANOPY_LIBURING_BOOTSTRAP_LIBRARY "${CANOPY_LIBURING_INSTALL_DIR}/lib/liburing.a")

  file(MAKE_DIRECTORY "${CANOPY_LIBURING_BOOTSTRAP_INCLUDE_DIR}")

  ExternalProject_Add(
    canopy_liburing_external
    URL "https://github.com/axboe/liburing/archive/refs/tags/liburing-${CANOPY_LIBURING_VERSION}.tar.gz"
    URL_HASH SHA256=456f5f882165630f0dc7b75e8fd53bd01a955d5d4720729b4323097e6e9f2a98
    PREFIX "${CANOPY_LIBURING_PREFIX}"
    BUILD_IN_SOURCE 1
    CONFIGURE_COMMAND
      <SOURCE_DIR>/configure --prefix=${CANOPY_LIBURING_INSTALL_DIR}
      --includedir=${CANOPY_LIBURING_BOOTSTRAP_INCLUDE_DIR} --libdir=${CANOPY_LIBURING_INSTALL_DIR}/lib
      --libdevdir=${CANOPY_LIBURING_INSTALL_DIR}/lib --cc=${CMAKE_C_COMPILER}
    BUILD_COMMAND ${CANOPY_MAKE_EXECUTABLE} -C <SOURCE_DIR>/src ENABLE_SHARED=0
    INSTALL_COMMAND
      ${CANOPY_MAKE_EXECUTABLE} -C <SOURCE_DIR>/src install ENABLE_SHARED=0 prefix=${CANOPY_LIBURING_INSTALL_DIR}
      includedir=${CANOPY_LIBURING_BOOTSTRAP_INCLUDE_DIR} libdir=${CANOPY_LIBURING_INSTALL_DIR}/lib
      libdevdir=${CANOPY_LIBURING_INSTALL_DIR}/lib
    BUILD_BYPRODUCTS "${CANOPY_LIBURING_BOOTSTRAP_LIBRARY}"
    LOG_DOWNLOAD ON
    LOG_CONFIGURE ON
    LOG_BUILD ON
    LOG_INSTALL ON)

  add_library(canopy_liburing STATIC IMPORTED)
  set_target_properties(
    canopy_liburing PROPERTIES IMPORTED_LOCATION "${CANOPY_LIBURING_BOOTSTRAP_LIBRARY}"
                               INTERFACE_INCLUDE_DIRECTORIES "${CANOPY_LIBURING_BOOTSTRAP_INCLUDE_DIR}")
  add_dependencies(canopy_liburing canopy_liburing_external)
  set(CANOPY_LIBURING_BOOTSTRAP_TARGET canopy_liburing_external)
  message(
    STATUS
      "liburing not found on system; will bootstrap liburing-${CANOPY_LIBURING_VERSION} into ${CANOPY_LIBURING_PREFIX}")
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
  PUBLIC rpc::rpc io_uring_config_idl
  PRIVATE libcoro canopy_liburing ${CANOPY_LIBRARIES} ${CANOPY_FMT_LIB})

if(CANOPY_LIBURING_BOOTSTRAP_TARGET)
  add_dependencies(canopy_io_uring ${CANOPY_LIBURING_BOOTSTRAP_TARGET})
endif()

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
