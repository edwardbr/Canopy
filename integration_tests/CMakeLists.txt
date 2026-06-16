#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

if(CANOPY_BUILD_TEST)
  add_subdirectory(interfaces)
  add_subdirectory(fuzz_test)
endif()
