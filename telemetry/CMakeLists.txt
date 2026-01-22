#
# Copyright (c) 2026 Edward Boggis-Rolfe All rights reserved.
#

cmake_minimum_required(VERSION 3.24)

if(CANOPY_USE_TELEMETRY)
  message("rpc_telemetry_interface")
  add_library(rpc_telemetry_interface INTERFACE)
  target_include_directories(rpc_telemetry_interface INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")

  add_library(rpc::rpc_telemetry_interface ALIAS rpc_telemetry_interface)

  if(CANOPY_BUILD_ENCLAVE)
    message("rpc_telemetry_enclave")
    add_library(rpc_telemetry_enclave STATIC src/enclave_telemetry_service.cpp)

    target_compile_definitions(rpc_telemetry_enclave PRIVATE ${CANOPY_ENCLAVE_DEFINES})
    target_include_directories(
      rpc_telemetry_enclave
      PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
      PRIVATE ${CANOPY_ENCLAVE_LIBCXX_INCLUDES})
    target_compile_options(rpc_telemetry_enclave PRIVATE ${CANOPY_ENCLAVE_COMPILE_OPTIONS} ${CANOPY_WARN_PEDANTIC})
    target_link_directories(rpc_telemetry_enclave PUBLIC ${SGX_LIBRARY_PATH})

    if(CANOPY_ENABLE_CLANG_TIDY)
      set_target_properties(rpc_telemetry_enclave PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
    endif()

    target_link_libraries(rpc_telemetry_enclave PRIVATE rpc_telemetry_edl_header_enclave yas_common rpc::rpc_enclave
                                                        ${CANOPY_ENCLAVE_FMT_LIB})

    set_property(TARGET rpc_telemetry_enclave PROPERTY COMPILE_PDB_NAME rpc_telemetry_enclave)

    add_library(rpc::rpc_telemetry_enclave ALIAS rpc_telemetry_enclave)
  endif()

endif()
if(${CANOPY_BUILD})

  message("rpc_telemetry")
  add_library(
    rpc_telemetry STATIC
    src/animation_telemetry_service.cpp
    src/sequence_diagram_telemetry_service.cpp
    src/console_telemetry_service.cpp
    src/telemetry_handler.cpp
    src/telemetry_service_factory.cpp
    src/multiplexing_telemetry_service.cpp)

  target_include_directories(rpc_telemetry PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
  target_compile_options(rpc_telemetry PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_PEDANTIC})
  target_link_options(rpc_telemetry PRIVATE ${CANOPY_LINK_OPTIONS})
  target_link_directories(rpc_telemetry PUBLIC ${SGX_LIBRARY_PATH})
  target_compile_definitions(rpc_telemetry PRIVATE ${CANOPY_DEFINES})
  target_link_libraries(
    rpc_telemetry
    PRIVATE rpc::rpc
            fmt::fmt-header-only
            yas_common
            spdlog::spdlog
            ${CANOPY_FMT_LIB})

  add_library(rpc::rpc_telemetry ALIAS rpc_telemetry)
  set_property(TARGET rpc_telemetry PROPERTY COMPILE_PDB_NAME rpc_telemetry)
endif()

install(
  DIRECTORY include/
  DESTINATION include/rpc
  FILES_MATCHING
  PATTERN "*.h*")
