#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]
cmake_minimum_required(VERSION 3.24)

add_subdirectory(idls)
add_subdirectory(common)
# add_subdirectory(memory_tests)
add_subdirectory(std_test)
add_subdirectory(fixtures)
add_subdirectory(blocking_dll_test_dll)
add_subdirectory(c_abi_dynamic_library_dll)
if(CANOPY_BUILD_PROTOCOL_BUFFERS AND CANOPY_BUILD_RUST)
  add_subdirectory(rust_dynamic_library_abi)
endif()
add_subdirectory(unshared_scheduler_dll_test_dll)
add_subdirectory(shared_scheduler_dll_test_dll)
add_subdirectory(ipc_spsc)
add_subdirectory(test_host)
add_subdirectory(serialiser_test)
add_subdirectory(json_schema_test)

if(CANOPY_BUILD_COROUTINE AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../subcomponents/file_system/tests file_system_tests)
endif()

# Add unit tests subdirectory
add_subdirectory(unit_tests)
