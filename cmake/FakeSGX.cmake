#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

if(NOT CANOPY_BUILD_ENCLAVE)
  message(FATAL_ERROR "FakeSGX.cmake should only be included when CANOPY_BUILD_ENCLAVE=ON")
endif()

if(NOT CANOPY_BUILD_COROUTINE)
  message(FATAL_ERROR "The fake SGX backend currently supports the coroutine SGX runtime only")
endif()

if(NOT CANOPY_BUILD_NANOPB)
  message(FATAL_ERROR "The fake SGX backend requires CANOPY_BUILD_NANOPB=ON for enclave marshalling")
endif()

set(CANOPY_BUILD_ENCLAVE_FLAG CANOPY_BUILD_ENCLAVE)
set(CANOPY_ENCLAVE_TARGET "FakeSGX")
set(CANOPY_FAKE_SGX_ROOT "${CMAKE_SOURCE_DIR}/c++/subcomponents/fake_sgx")
set(CANOPY_FAKE_SGX_INCLUDE_DIR "${CANOPY_FAKE_SGX_ROOT}/include")
set(CANOPY_FAKE_SGX_TRUSTED_INCLUDE_DIR "${CANOPY_FAKE_SGX_ROOT}/trusted_include")
set(SGX_INCLUDE_DIR "${CANOPY_FAKE_SGX_INCLUDE_DIR}")
set(SGX_LIBRARY_PATH "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}")
set(SGX_ENCLAVE_LIBS)

set(CANOPY_ENCLAVE_FMT_LIB fmt::fmt-header-only)
set(CANOPY_ENCLAVE_PROTOBUF_TARGET)
set(CANOPY_ENCLAVE_PROTOBUF_DEFINES CANOPY_BUILD_NANOPB CANOPY_USE_NANOPB_FOR_PROTOCOL_BUFFERS)

set(CANOPY_SHARED_FAKE_SGX_DEFINES
    FOR_SGX
    CANOPY_FAKE_SGX
    ${CANOPY_SHARED_DEFINES}
    ENCLAVE_STATUS=sgx_status_t
    ENCLAVE_OK=SGX_SUCCESS
    DISALLOW_BAD_JUMPS)
list(REMOVE_ITEM CANOPY_SHARED_FAKE_SGX_DEFINES CANOPY_BUILD_PROTOCOL_BUFFERS CANOPY_USE_PROTOCOL_BUFFERS_FOR_NANOPB)
list(APPEND CANOPY_SHARED_FAKE_SGX_DEFINES ${CANOPY_ENCLAVE_PROTOBUF_DEFINES})
list(REMOVE_DUPLICATES CANOPY_SHARED_FAKE_SGX_DEFINES)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
  set(CANOPY_ENCLAVE_DEFINES ${CANOPY_SHARED_FAKE_SGX_DEFINES} NDEBUG)
  set(CANOPY_SGX_CREATE_ENCLAVE_DEBUG_FLAG 0)
else()
  set(CANOPY_ENCLAVE_DEFINES ${CANOPY_SHARED_FAKE_SGX_DEFINES} SGX_DEBUG=1 _DEBUG)
  set(CANOPY_SGX_CREATE_ENCLAVE_DEBUG_FLAG 1)
endif()

set(CANOPY_ENCLAVE_COMPILE_OPTIONS ${CANOPY_COMPILE_OPTIONS} -Wno-c++17-extensions)
set(CANOPY_ENCLAVE_LINK_OPTIONS ${CANOPY_LINK_DYNAMIC_LIBRARY_OPTIONS})
set(CANOPY_WARN_PEDANTIC_ENCLAVE ${CANOPY_WARN_PEDANTIC} "SHELL:-Wno-variadic-macros"
                                 "SHELL:-Wno-gnu-include-next")
set(CANOPY_WARN_OK_ENCLAVE ${CANOPY_WARN_OK} "SHELL:-Wno-variadic-macros" "SHELL:-Wno-gnu-include-next")

set(CANOPY_ENCLAVE_POLYFILL_INCLUDES ${CMAKE_SOURCE_DIR}/c++/rpc/include/rpc/internal/polyfill/sgx)
set(CANOPY_ENCLAVE_LIBCXX_INCLUDES ${CANOPY_ENCLAVE_POLYFILL_INCLUDES} ${CANOPY_FAKE_SGX_TRUSTED_INCLUDE_DIR}
                                   ${CANOPY_FAKE_SGX_INCLUDE_DIR})

set(CANOPY_INCLUDES ${SGX_INCLUDE_DIR})
set(CANOPY_SGX_HOST_LIBRARY_DIRS "${SGX_LIBRARY_PATH}")
set(CANOPY_SGX_HOST_LINK_OPTIONS)

function(canopy_configure_sgxssl_for_enclave)
  # Fake SGX does not need Intel SGXSSL to exercise the enclave runtime. Keep the command available so SGXSSL consumers
  # can share one CMake path, but do not pretend the real SGXSSL trusted archives are present.
  set(CANOPY_SGXSSL_HEADERS_READY
      FALSE
      PARENT_SCOPE)
  set(CANOPY_SGXSSL_IMPL_LIBS_FOUND
      FALSE
      PARENT_SCOPE)
  set(CANOPY_SGXSSL_ENCLAVE_LINK_LIBRARIES
      ""
      PARENT_SCOPE)
  set(CANOPY_STREAMING_TLS_ENCLAVE_HAS_SGXSSL_IMPL
      FALSE
      CACHE INTERNAL "True when streaming_tls_enclave links SGXSSL implementation archives" FORCE)
endfunction()

add_library(canopy_fake_sgx_runtime SHARED "${CANOPY_FAKE_SGX_ROOT}/src/fake_sgx.cpp")
target_compile_definitions(canopy_fake_sgx_runtime PRIVATE ${CANOPY_DEFINES})
target_include_directories(canopy_fake_sgx_runtime PUBLIC "$<BUILD_INTERFACE:${CANOPY_FAKE_SGX_INCLUDE_DIR}>")
target_compile_options(canopy_fake_sgx_runtime PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(canopy_fake_sgx_runtime PRIVATE ${CANOPY_LINK_DYNAMIC_LIBRARY_OPTIONS})
target_link_libraries(canopy_fake_sgx_runtime PRIVATE ${CMAKE_DL_LIBS})
set_property(TARGET canopy_fake_sgx_runtime PROPERTY COMPILE_PDB_NAME canopy_fake_sgx_runtime)

set(CANOPY_SGX_HOST_LIBRARIES canopy_fake_sgx_runtime)

function(
  host_edl_library
  target
  edl
  edl_search_paths
  use_prefix
  edl_include_path
  edl_link_libraries)
  cmake_parse_arguments(
    "FAKE_SGX"
    ""
    ""
    "EXTRA_DEPENDS;EXTRA_LIBS"
    ${ARGN})

  if(NOT "${target}" STREQUAL "canopy_coroutine_edl")
    message(FATAL_ERROR "The fake SGX backend only provides the canopy_coroutine_enclave EDL shim")
  endif()

  add_library(${target} STATIC "${CANOPY_FAKE_SGX_ROOT}/src/canopy_coroutine_enclave_u.cpp")
  target_include_directories(
    ${target}
    PUBLIC "$<BUILD_INTERFACE:${CANOPY_FAKE_SGX_INCLUDE_DIR}>"
    PRIVATE ${CMAKE_BINARY_DIR}/generated/include ${CMAKE_BINARY_DIR}/generated/src ${edl_search_paths}
            ${edl_include_path})
  target_compile_definitions(${target} PRIVATE ${CANOPY_DEFINES} LIBCORO_FEATURE_NETWORKING)
  target_compile_options(${target} PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
  target_link_options(${target} PRIVATE ${CANOPY_LINK_OPTIONS})
  target_link_libraries(
    ${target}
    PRIVATE canopy_fake_sgx_runtime
            yas_common
            ${CANOPY_LIBRARIES}
            ${edl_link_libraries}
            ${FAKE_SGX_EXTRA_LIBS})
  add_custom_target(${target}-create-header ALL
                    DEPENDS ${CANOPY_FAKE_SGX_ROOT}/include/untrusted/canopy_coroutine_enclave_u.h)
  add_dependencies(${target} ${target}-create-header)
  if(TARGET secure_coroutine_module_idl)
    add_dependencies(${target} secure_coroutine_module_idl)
  endif()
endfunction()

function(
  enclave_edl_library
  target
  edl
  edl_search_paths
  use_prefix
  edl_include_path
  edl_link_libraries)
  cmake_parse_arguments(
    "FAKE_SGX"
    ""
    ""
    "EXTRA_DEPENDS"
    ${ARGN})

  if(NOT "${target}" STREQUAL "canopy_coroutine_edl_enclave")
    message(FATAL_ERROR "The fake SGX backend only provides the canopy_coroutine_enclave trusted EDL shim")
  endif()

  add_library(${target} STATIC "${CANOPY_FAKE_SGX_ROOT}/src/empty.cpp")
  target_include_directories(
    ${target}
    PUBLIC "$<BUILD_INTERFACE:${CANOPY_FAKE_SGX_INCLUDE_DIR}>"
    PRIVATE ${CANOPY_ENCLAVE_LIBCXX_INCLUDES} ${edl_search_paths} ${edl_include_path})
  target_compile_definitions(${target} PRIVATE ${CANOPY_ENCLAVE_DEFINES})
  target_compile_options(${target} PRIVATE ${CANOPY_ENCLAVE_COMPILE_OPTIONS} ${CANOPY_WARN_OK_ENCLAVE})
  target_link_libraries(${target} PRIVATE ${edl_link_libraries})
endfunction()

function(add_untrusted_header target)
  add_library(${target} INTERFACE)
  target_include_directories(${target} INTERFACE "$<BUILD_INTERFACE:${CANOPY_FAKE_SGX_INCLUDE_DIR}>")
endfunction()

function(add_trusted_header target)
  add_library(${target} INTERFACE)
  target_include_directories(${target} INTERFACE "$<BUILD_INTERFACE:${CANOPY_FAKE_SGX_TRUSTED_INCLUDE_DIR}>"
                                                 "$<BUILD_INTERFACE:${CANOPY_FAKE_SGX_INCLUDE_DIR}>")
endfunction()

function(add_enclave_library target)
  set(optionArgs USE_PREFIX REMOVE_INIT_SECTION)
  set(oneValueArgs EDL_IMPL LDSCRIPT)
  set(multiValueArgs
      SRCS
      TRUSTED_LIBS
      HEADER_ONLY_LIBS
      EDL
      EDL_SEARCH_PATHS
      EDL_INCLUDE_PATH)
  cmake_parse_arguments(
    "FAKE_SGX"
    "${optionArgs}"
    "${oneValueArgs}"
    "${multiValueArgs}"
    ${ARGN})

  add_library(${target} SHARED ${FAKE_SGX_SRCS})
  target_include_directories(${target} PRIVATE ${CANOPY_ENCLAVE_LIBCXX_INCLUDES})
  target_compile_definitions(${target} PRIVATE ${CANOPY_ENCLAVE_DEFINES})
  target_compile_options(${target} PRIVATE ${CANOPY_ENCLAVE_COMPILE_OPTIONS} ${CANOPY_WARN_OK_ENCLAVE})
  target_link_options(${target} PRIVATE ${CANOPY_ENCLAVE_LINK_OPTIONS})
  target_link_libraries(${target} PRIVATE ${FAKE_SGX_TRUSTED_LIBS} ${FAKE_SGX_HEADER_ONLY_LIBS} canopy_fake_sgx_runtime)
endfunction()

function(enclave_sign target)
  set(oneValueArgs KEY CONFIG OUTPUT)
  cmake_parse_arguments(
    "FAKE_SGX"
    ""
    "${oneValueArgs}"
    ""
    ${ARGN})

  if("${FAKE_SGX_OUTPUT}" STREQUAL "")
    if(WIN32)
      set(OUTPUT_NAME "${target}.signed.dll")
    else()
      set(OUTPUT_NAME "lib${target}.signed.so")
    endif()
  else()
    set(OUTPUT_NAME ${FAKE_SGX_OUTPUT})
  endif()

  set(${target}_sign_OUTPUT_NAME
      ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/${OUTPUT_NAME}
      CACHE STRING "signed fake SGX target file name")

  add_custom_command(
    TARGET ${target}
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:${target}> $<TARGET_FILE_DIR:${target}>/${OUTPUT_NAME}
    VERBATIM)

  set_property(
    DIRECTORY
    APPEND
    PROPERTY ADDITIONAL_MAKE_CLEAN_FILES "$<TARGET_FILE_DIR:${target}>/${OUTPUT_NAME}")
endfunction()

message(STATUS "Fake SGX coroutine backend configured")
message("CANOPY_ENCLAVE_DEFINES ${CANOPY_ENCLAVE_DEFINES}")
message("CANOPY_ENCLAVE_COMPILE_OPTIONS ${CANOPY_ENCLAVE_COMPILE_OPTIONS}")
message("CANOPY_ENCLAVE_LINK_OPTIONS ${CANOPY_ENCLAVE_LINK_OPTIONS}")
