cmake_minimum_required(VERSION 3.24)

add_executable(rest_client_generation_test rest_client_generation_test.cpp)
target_compile_definitions(rest_client_generation_test PRIVATE ${CANOPY_DEFINES})
target_compile_options(rest_client_generation_test PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(rest_client_generation_test PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
target_link_libraries(
  rest_client_generation_test
  PRIVATE GTest::gtest_main
          rest_fixture_idl
          streaming_http_client
          rest
          json
          streaming
          rpc_log)

add_test(NAME rest_client_generation_test COMMAND rest_client_generation_test)
