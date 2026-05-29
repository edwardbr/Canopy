#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

add_subdirectory(core)
# tcp, websocket and tls (OpenSSL) are dual-mode. mbedtls/spsc/attestation are still coroutine-only.
add_subdirectory(tcp)
if(CANOPY_BUILD_WEBSOCKET)
  add_subdirectory(websocket)
endif()
if(CANOPY_SECURE_STREAM_BACKEND STREQUAL "OPENSSL")
  add_subdirectory(tls)
endif()

if(CANOPY_BUILD_COROUTINE)
  if(CANOPY_BUILD_MBEDTLS AND TARGET canopy_mbedtls_tls)
    add_subdirectory(mbedtls)
  endif()
  add_subdirectory(spsc_queue)
  add_subdirectory(spsc_wrapping)
  add_subdirectory(attestation)
endif()

if(CANOPY_BUILD_TEST
   AND CMAKE_SYSTEM_NAME STREQUAL "Linux"
   AND CANOPY_BUILD_COROUTINE)
  add_executable(streaming_regression_test tests/streaming_regression_test.cpp)
  target_include_directories(streaming_regression_test PRIVATE ${CANOPY_INCLUDES})
  target_compile_definitions(streaming_regression_test PRIVATE ${CANOPY_DEFINES})
  target_compile_options(streaming_regression_test PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
  target_link_options(streaming_regression_test PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
  target_link_libraries(streaming_regression_test PRIVATE streaming streaming_spsc_wrapping GTest::gtest_main rpc_log)
  add_test(NAME streaming_regression_test COMMAND streaming_regression_test)
  message(STATUS "streaming_regression_test configured")
endif()

message(STATUS "streaming configured")
