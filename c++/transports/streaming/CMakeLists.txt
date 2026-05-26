#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

# transport_streaming is dual-mode. Coroutine builds use libcoro; blocking
# builds dispatch the three loops to rpc::blocking_executor via the SPAWN
# macro and use rpc::event for completion signalling.
add_library(transport_streaming src/transport.cpp)

target_compile_definitions(transport_streaming PRIVATE ${CANOPY_DEFINES})

target_include_directories(
  transport_streaming
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include>"
  PRIVATE ${CANOPY_INCLUDES})

if(CANOPY_BUILD_COROUTINE)
  target_link_libraries(
    transport_streaming
    PUBLIC streaming
    PRIVATE yas_common
            streaming_transport_idl
            rpc_types_idl
            libcoro
            ${CANOPY_LIBRARIES})
else()
  target_link_libraries(
    transport_streaming
    PUBLIC streaming
    PRIVATE yas_common
            streaming_transport_idl
            rpc_types_idl
            ${CANOPY_LIBRARIES})
endif()

target_compile_options(transport_streaming PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(transport_streaming PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
set_property(TARGET transport_streaming PROPERTY COMPILE_PDB_NAME transport_streaming)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(transport_streaming PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()

if(CANOPY_BUILD_COROUTINE)

  if(CANOPY_BUILD_ENCLAVE)
    add_library(transport_streaming_enclave src/transport.cpp)

    target_compile_definitions(transport_streaming_enclave PRIVATE ${CANOPY_ENCLAVE_DEFINES})

    target_include_directories(
      transport_streaming_enclave
      PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include>"
      PRIVATE ${CANOPY_ENCLAVE_LIBCXX_INCLUDES})

    target_link_libraries(
      transport_streaming_enclave
      PUBLIC streaming_transport_idl_enclave
      PRIVATE streaming_enclave yas_common rpc_types_idl_enclave rpc::rpc_enclave)

    target_compile_options(transport_streaming_enclave PRIVATE ${CANOPY_ENCLAVE_COMPILE_OPTIONS}
                                                               ${CANOPY_WARN_OK_ENCLAVE})
    target_link_options(transport_streaming_enclave PRIVATE ${CANOPY_ENCLAVE_LINK_OPTIONS})
    set_property(TARGET transport_streaming_enclave PROPERTY COMPILE_PDB_NAME transport_streaming_enclave)

    if(CANOPY_ENABLE_CLANG_TIDY)
      set_target_properties(transport_streaming_enclave PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
    endif()
  endif()

  add_library(transport_streaming_test INTERFACE)
  target_include_directories(transport_streaming_test INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/tests>")
  target_link_libraries(transport_streaming_test INTERFACE streaming streaming_tcp streaming_spsc_queue)

endif()
