#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]
cmake_minimum_required(VERSION 3.24)

# Add server
add_subdirectory(idl)
add_subdirectory(server)
if(CANOPY_BUILD_TEST)
  canopy_add_subdirectory_without_clang_tidy(tests)
endif()
