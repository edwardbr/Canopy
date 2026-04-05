#
# Copyright (c) 2026 Edward Boggis-Rolfe All rights reserved.
#

cmake_minimum_required(VERSION 3.24)

find_package(OpenSSL REQUIRED)

add_library(streaming_tls STATIC src/stream.cpp)
target_include_directories(
  streaming_tls
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include>"
  PRIVATE ${CANOPY_INCLUDES})
target_compile_definitions(streaming_tls PRIVATE ${CANOPY_DEFINES})
target_compile_options(streaming_tls PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(streaming_tls PRIVATE ${CANOPY_LINK_OPTIONS})
target_link_libraries(
  streaming_tls
  PUBLIC streaming OpenSSL::Crypto OpenSSL::SSL
  PRIVATE rpc libcoro)
if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(streaming_tls PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()
