#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]
cmake_minimum_required(VERSION 3.24)

add_subdirectory(rpc_objects)

# WebSocket demo: dual-mode in plain-HTTP configurations; HTTPS still requires coroutine mode until streaming::secure
# becomes dual-mode.
if(CANOPY_BUILD_WEBSOCKET)
  add_subdirectory(websocket)
endif()
# stream_composition still composes coroutine-only impls (SPSC, TLS).
if(CANOPY_BUILD_COROUTINE)
  add_subdirectory(stream_composition)
endif()
add_subdirectory(config_demo)
add_subdirectory(comprehensive)
add_subdirectory(llm_mcp)
