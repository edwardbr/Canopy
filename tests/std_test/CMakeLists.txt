#[[
  Copyright (c) 2026 Edward Boggis-Rolfe
  All rights reserved.
]]
cmake_minimum_required(VERSION 3.24)

include(CTest)
include(GoogleTest)

# Build individual shared_ptr/weak_ptr tests with resolved dependencies These tests now have access to test
# infrastructure headers

# Tests with resolved dependencies that should now build
set(BUILDABLE_TESTS
    # P0718R2_atomic_smart_ptrs                     # C++20 only - atomic<shared_ptr> specializations
    # P1020R1_smart_pointer_for_overwrite           # C++20 only - make_shared_for_overwrite not in C++17
    # P1135R6_atomic_wait                           # C++20 only - atomic<shared_ptr> specializations
    # LWG3018_shared_ptr_function                   # shared_ptr function tests disable as we are not going to implement
    # that
    # Dev10_498944_enable_shared_from_this_auto_ptr # not interested in auto_ptr - C++17 removed auto_ptr
    Dev09_196243_tr1_enable_shared_from_this_ops # enable_shared_from_this tests
    Dev10_445289_make_shared # make_shared tests
    Dev10_654977_655012_shared_ptr_move # shared_ptr move semantics
    # Dev11_1066589_shared_ptr_atomic_deadlock     # shared_ptr atomic operations - requires C++20 atomic<shared_ptr>
    Dev10_851347_weak_ptr_virtual_inheritance
    Dev09_153419_tr1_allocators
    Dev09_174589_tr1_function_storing_pmf_called_with_reference_or_pointer
    Dev11_0376122_grand_theft_bind
    Dev11_0920385_list_sort_allocator
    P2162R2_std_visit_for_derived_classes_from_variant
    VSO_0000000_has_static_rtti
    VSO_0000000_initialize_everything
    VSO_0105317_expression_sfinae
    Dev10_847656_shared_ptr_is_convertible
    Dev10_500860_overloaded_address_of
    Dev10_635436_shared_ptr_reset
    Dev10_722102_shared_ptr_nullptr # Array type SFINAE constraints not implemented
    Dev11_0000000_user_defined_literals
    Dev11_0535636_functional_overhaul
    GH_002299_implicit_sfinae_constraints # Advanced SFINAE constraints not fully implemented
    # GH_005315_destructor_tombstones   # Built standalone below: main(argc,argv) + death-test framework incompatible
    # with GTest wrapper
    P0433R2_deduction_guides # Conflicts with std library headers
    P0513R0_poisoning_the_hash
    VSO_0000000_instantiate_iterators_misc # Conflicts with std library headers
)

foreach(test_name ${BUILDABLE_TESTS})
  set(test_dir "${CMAKE_CURRENT_SOURCE_DIR}/tests/${test_name}")
  if(EXISTS "${test_dir}")
    # Find source files
    file(GLOB test_sources "${test_dir}/*.cpp" "${test_dir}/*.c")

    if(test_sources)
      # Generate a GTest wrapper that calls the test's main() (renamed to std_test_main via -D). This lets TestMate
      # discover and run each test as a named GTest case.
      set(generated_wrapper "${CMAKE_CURRENT_BINARY_DIR}/${test_name}_gtest_wrapper.cpp")
      set(TEST_NAME ${test_name})
      configure_file("${CMAKE_CURRENT_SOURCE_DIR}/gtest_wrapper.cpp.in" "${generated_wrapper}" @ONLY)

      add_executable(${test_name} ${test_sources} ${generated_wrapper})

      target_include_directories(
        ${test_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include # STL test infrastructure
                             ${CMAKE_CURRENT_SOURCE_DIR}/../../rpc/include # For remote_pointer.h
      )

      # Rename main() to std_test_main() so GTest::gtest_main can supply the real entry point
      target_compile_definitions(
        ${test_name} PRIVATE _SILENCE_ALL_MS_EXT_DEPRECATION_WARNINGS # Suppress MS-specific warnings
                             TEST_STL_COMPLIANCE "main=std_test_main")

      # Enable destructor tombstones for GH_005315 test
      if(test_name STREQUAL "GH_005315_destructor_tombstones")
        target_compile_definitions(${test_name} PRIVATE _MSVC_STL_DESTRUCTOR_TOMBSTONES=1)
      endif()

      # Force include compatibility headers for Microsoft-specific functions
      if(NOT MSVC)
        target_compile_options(${test_name} PRIVATE "-include${CMAKE_CURRENT_SOURCE_DIR}/include/msvc_compat.hpp")
        target_compile_options(${test_name} PRIVATE -Wno-unqualified-std-cast-call)
      endif()

      # Link atomic library for Clang 18 compatibility
      if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS "19")
        target_link_libraries(${test_name} PRIVATE atomic)
      endif()

      target_link_libraries(${test_name} PRIVATE GTest::gtest_main)
      target_compile_features(${test_name} PRIVATE cxx_std_17)

      # Force C++17 standard (override any C++20 settings)
      set_target_properties(${test_name} PROPERTIES CXX_STANDARD 17)

      # ASan options: disable leak detection for all tests; Dev10_445289_make_shared also needs alloc_dealloc_mismatch=0
      # because it overrides operator new/delete
      if(test_name STREQUAL "Dev10_445289_make_shared")
        set(asan_env "ASAN_OPTIONS=detect_leaks=0:alloc_dealloc_mismatch=0;LSAN_OPTIONS=detect_leaks=0")
      else()
        set(asan_env "ASAN_OPTIONS=detect_leaks=0;LSAN_OPTIONS=detect_leaks=0")
      endif()
      gtest_discover_tests(${test_name} PROPERTIES ENVIRONMENT "${asan_env}")
    endif()
  endif()
endforeach()

# GH_005315_destructor_tombstones uses main(int,char**) and a death-test framework that is incompatible with the GTest
# wrapper pattern above. Build it as a standalone executable with its own entry point.
set(tombstones_dir "${CMAKE_CURRENT_SOURCE_DIR}/tests/GH_005315_destructor_tombstones")
if(EXISTS "${tombstones_dir}")
  file(GLOB tombstones_sources "${tombstones_dir}/*.cpp" "${tombstones_dir}/*.c")
  if(tombstones_sources)
    add_executable(GH_005315_destructor_tombstones ${tombstones_sources})
    target_include_directories(GH_005315_destructor_tombstones PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
                                                                       ${CMAKE_CURRENT_SOURCE_DIR}/../../rpc/include)
    target_compile_definitions(
      GH_005315_destructor_tombstones PRIVATE _SILENCE_ALL_MS_EXT_DEPRECATION_WARNINGS TEST_STL_COMPLIANCE
                                              _MSVC_STL_DESTRUCTOR_TOMBSTONES=1)
    if(NOT MSVC)
      target_compile_options(
        GH_005315_destructor_tombstones PRIVATE "-include${CMAKE_CURRENT_SOURCE_DIR}/include/msvc_compat.hpp"
                                                -Wno-unqualified-std-cast-call)
    endif()
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS "19")
      target_link_libraries(GH_005315_destructor_tombstones PRIVATE atomic)
    endif()
    target_compile_features(GH_005315_destructor_tombstones PRIVATE cxx_std_17)
    set_target_properties(GH_005315_destructor_tombstones PROPERTIES CXX_STANDARD 17)
    add_test(NAME GH_005315_destructor_tombstones COMMAND GH_005315_destructor_tombstones)
    set_tests_properties(GH_005315_destructor_tombstones
                         PROPERTIES ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0;LSAN_OPTIONS=detect_leaks=0")
  endif()
endif()
