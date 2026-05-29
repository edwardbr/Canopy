# Copyright (c) 2026 Edward Boggis-Rolfe All rights reserved.

cmake_minimum_required(VERSION 3.24)

if(NOT CANOPY_BUILD_COROUTINE)
  add_executable(blocking_executor_test blocking_executor_test.cpp)

  target_link_libraries(blocking_executor_test GTest::gtest_main rpc::rpc)

  target_include_directories(blocking_executor_test PRIVATE ${CANOPY_CPP_SOURCE_DIR}/rpc/include
                                                            ${CMAKE_BINARY_DIR}/generated/include)

  target_compile_options(blocking_executor_test PRIVATE ${CANOPY_COMPILE_OPTIONS})
  target_link_options(blocking_executor_test PRIVATE ${CANOPY_LINK_EXE_OPTIONS})

  add_test(NAME blocking_executor_test COMMAND blocking_executor_test)

  if(CANOPY_ENABLE_CLANG_TIDY)
    set_target_properties(blocking_executor_test PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
  endif()
endif()
