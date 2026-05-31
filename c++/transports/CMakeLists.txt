#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

add_subdirectory(direct)
add_subdirectory(c_abi)
add_subdirectory(dynamic_library)
add_subdirectory(unshared_scheduler_dll)
add_subdirectory(shared_scheduler_dll)
add_subdirectory(common)
add_subdirectory(ipc_spsc)
add_subdirectory(local)
add_subdirectory(streaming)
if(CANOPY_BUILD_WEBSOCKET)
  add_subdirectory(websocket)
endif()
add_subdirectory(mock_test)

if(CANOPY_BUILD_ENCLAVE)
  add_subdirectory(sgx_common)
  add_subdirectory(sgx_blocking)
endif()

if(CANOPY_BUILD_ENCLAVE AND CANOPY_BUILD_COROUTINE)
  add_subdirectory(sgx_coroutine)
endif()
