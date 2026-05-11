#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]
cmake_minimum_required(VERSION 3.24)

if(CANOPY_BUILD_ENCLAVE)
  set(CANOPY_DEFINES ${CANOPY_DEFINES} CANOPY_BUILD_ENCLAVE)
endif()

add_subdirectory(idls)
add_subdirectory(common)
# add_subdirectory(memory_tests)
add_subdirectory(std_test)
add_subdirectory(fixtures)
add_subdirectory(dynamic_library_dll)
add_subdirectory(c_abi_dynamic_library_dll)
if(CANOPY_BUILD_PROTOCOL_BUFFERS)
  add_subdirectory(rust_dynamic_library_abi)
endif()
add_subdirectory(libcoro_dll_scheduled_dynamic_library_dll)
add_subdirectory(libcoro_host_scheduled_dynamic_library_dll)
add_subdirectory(libcoro_spsc_dynamic_dll)
add_subdirectory(test_host)
add_subdirectory(serialiser_test)
# this is a way to detect if we have nlohman that we can use for testing json schemas
if(DEFINED NLOHMANN_JSON_CONFIG_INSTALL_DIR)
  add_subdirectory(json_schema_test)
endif()

if(CANOPY_BUILD_ENCLAVE)
  add_subdirectory(test_enclave)
endif()

if(CANOPY_BUILD_ENCLAVE AND CANOPY_BUILD_COROUTINE)
  add_subdirectory(sgx_coroutine_test_enclave)
  add_subdirectory(sgx_coroutine_test_host)
endif()

if(CANOPY_BUILD_COROUTINE AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../subcomponents/file_system/tests file_system_tests)
endif()

# Add unit tests subdirectory
add_subdirectory(unit_tests)
