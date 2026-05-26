#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]
cmake_minimum_required(VERSION 3.24)

# WebSocket demo: dual-mode in plain-HTTP configurations; HTTPS still requires
# coroutine mode until streaming::secure becomes dual-mode.
if(CANOPY_BUILD_WEBSOCKET)
  add_subdirectory(websocket)
endif()
# stream_composition still composes coroutine-only impls (SPSC, TLS).
if(CANOPY_BUILD_COROUTINE)
  add_subdirectory(stream_composition)
endif()
add_subdirectory(comprehensive)
