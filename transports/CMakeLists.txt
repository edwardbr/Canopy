#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

add_subdirectory(direct)
add_subdirectory(local)
add_subdirectory(streaming)
add_subdirectory(mock_test)

if(CANOPY_BUILD_ENCLAVE)
  add_subdirectory(sgx)
endif()
