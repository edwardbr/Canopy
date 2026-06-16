#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

if(CANOPY_BUILD_COROUTINE AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_subdirectory(file_system_test_host)
endif()
