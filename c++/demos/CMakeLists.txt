#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]
cmake_minimum_required(VERSION 3.24)

# Add server
if(CANOPY_BUILD_COROUTINE)
  if(CANOPY_BUILD_WEBSOCKET)
    add_subdirectory(websocket)
  endif()
  add_subdirectory(stream_composition)
endif()
add_subdirectory(comprehensive)
