#
# Copyright (c) 2026 Edward Boggis-Rolfe All rights reserved.
#

cmake_minimum_required(VERSION 3.24)

if(CANOPY_USE_TELEMETRY)
  message("rpc_telemetry_interface")
  add_library(rpc_telemetry_interface INTERFACE)
  target_include_directories(rpc_telemetry_interface INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")

  add_library(rpc::rpc_telemetry_interface ALIAS rpc_telemetry_interface)
endif()

if(${CANOPY_USE_TELEMETRY})

  message("rpc_telemetry")
  add_library(
    rpc_telemetry STATIC
    src/animation_telemetry_service.cpp
    src/sequence_diagram_telemetry_service.cpp
    src/console_telemetry_service.cpp
    # src/telemetry_handler.cpp
    src/telemetry_service_factory.cpp
    src/multiplexing_telemetry_service.cpp)

  target_include_directories(rpc_telemetry PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
  target_compile_options(rpc_telemetry PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_PEDANTIC}
                                               $<$<CXX_COMPILER_ID:GNU>:-Wno-dangling-reference>)
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
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/rpc
  FILES_MATCHING
  PATTERN "*.h*")
