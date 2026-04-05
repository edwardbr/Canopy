#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

add_subdirectory(fullstack)
add_subdirectory(serialisation)
if(CANOPY_BUILD_COROUTINE)
  add_subdirectory(streaming)
endif()
