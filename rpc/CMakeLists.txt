#[[
   Copyright (c) 2024 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

add_subdirectory(interfaces)

if(CANOPY_USE_TELEMETRY)
  set(CANOPY_ENCLAVE_TELEMETRY_SERVICE rpc::rpc_telemetry_enclave)
  set(CANOPY_TELEMETRY_SERVICE rpc::rpc_telemetry)
  set(CANOPY_TELEMETRY_INTERFACE rpc::rpc_telemetry_interface)
else()
  set(CANOPY_ENCLAVE_TELEMETRY_SERVICE)
  set(CANOPY_TELEMETRY_SERVICE)
  set(CANOPY_TELEMETRY_INTERFACE)
endif()

if(CANOPY_BUILD_ENCLAVE)
  add_library(
    rpc_enclave
    include/rpc/internal/assert.h
    include/rpc/internal/casting_interface.h
    include/rpc/internal/error_codes.h
    include/rpc/internal/logger.h
    include/rpc/internal/marshaller.h
    include/rpc/internal/object_proxy.h
    include/rpc/internal/pass_through.h
    include/rpc/internal/serialiser.h
    include/rpc/internal/service_proxy.h
    include/rpc/internal/service.h
    include/rpc/internal/stub.h
    include/rpc/internal/types.h
    include/rpc/internal/transport.h
    include/rpc/internal/version.h
    src/casting_interface.cpp
    src/error_codes.cpp
    src/object_proxy.cpp
    src/pass_through.cpp
    src/service_proxy.cpp
    src/service.cpp
    src/stub.cpp
    src/transport.cpp
    src/version.cpp)

  target_compile_definitions(rpc_enclave PRIVATE ${CANOPY_ENCLAVE_DEFINES})
  target_compile_options(rpc_enclave PRIVATE ${CANOPY_ENCLAVE_COMPILE_OPTIONS} ${CANOPY_WARN_PEDANTIC})
  target_include_directories(
    rpc_enclave
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include/rpc>"
    PRIVATE ${CANOPY_ENCLAVE_LIBCXX_INCLUDES})
  target_link_libraries(
    rpc_enclave
    PUBLIC ${CANOPY_TELEMETRY_INTERFACE} ${CANOPY_ENCLAVE_TELEMETRY_SERVICE} ${CANOPY_ENCLAVE_FMT_LIB}
           rpc_types_idl_enclave
    PRIVATE yas_common)
  target_link_options(rpc_enclave PRIVATE ${CANOPY_ENCLAVE_LINK_OPTIONS})

  set_property(TARGET rpc_enclave PROPERTY COMPILE_PDB_NAME rpc_enclave)
  if(CANOPY_ENABLE_CLANG_TIDY)
    set_target_properties(rpc_enclave PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
  endif()
  add_library(rpc::rpc_enclave ALIAS rpc_enclave)
endif()

# ######################################################################################################################
add_library(
  rpc
  include/rpc/internal/assert.h
  include/rpc/internal/casting_interface.h
  include/rpc/internal/error_codes.h
  include/rpc/internal/logger.h
  include/rpc/internal/marshaller.h
  include/rpc/internal/object_proxy.h
  include/rpc/internal/pass_through.h
  include/rpc/internal/serialiser.h
  include/rpc/internal/service_proxy.h
  include/rpc/internal/service.h
  include/rpc/internal/stub.h
  include/rpc/internal/transport.h
  include/rpc/internal/types.h
  include/rpc/internal/version.h
  src/casting_interface.cpp
  src/error_codes.cpp
  src/object_proxy.cpp
  src/pass_through.cpp
  src/service_proxy.cpp
  src/service.cpp
  src/stub.cpp
  src/transport.cpp
  src/version.cpp)

# Conditionally add thread-local logger implementation
if(CANOPY_USE_THREAD_LOCAL_LOGGING)
  target_sources(rpc PRIVATE src/thread_local_logger.cpp)
endif()

target_compile_definitions(rpc PUBLIC ${CANOPY_DEFINES})

target_include_directories(
  rpc
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include/rpc>"
  PRIVATE ${CANOPY_INCLUDES})

if(CANOPY_BUILD_COROUTINE)
  set(CORO_RUNTIME libcoro)
endif()

target_link_libraries(
  rpc
  PUBLIC ${CANOPY_TELEMETRY_INTERFACE} ${CANOPY_TELEMETRY_SERVICE} ${CORO_RUNTIME} rpc_types_idl
  PRIVATE yas_common ${CANOPY_LIBRARIES} ${CANOPY_FMT_LIB})

target_compile_options(rpc PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_PEDANTIC})
target_link_options(rpc PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
target_link_directories(rpc PUBLIC ${SGX_LIBRARY_PATH})
set_property(TARGET rpc PROPERTY COMPILE_PDB_NAME rpc)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(rpc PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()

add_library(rpc::rpc ALIAS rpc)

install(
  DIRECTORY include/
  DESTINATION include/rpc
  FILES_MATCHING
  PATTERN "*.h*"
  # PATTERN "private" EXCLUDE
)
