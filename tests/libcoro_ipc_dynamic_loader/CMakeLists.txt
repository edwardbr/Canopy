#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

if(NOT CANOPY_BUILD_COROUTINE)
  return()
endif()

add_executable(canopy_libcoro_ipc_loader src/main.cpp ../libcoro_ipc_dynamic_dll/src/rpc_log.cpp)

target_compile_definitions(canopy_libcoro_ipc_loader PRIVATE ${CANOPY_DEFINES})

target_include_directories(canopy_libcoro_ipc_loader PRIVATE ${CANOPY_INCLUDES})

target_link_libraries(
  canopy_libcoro_ipc_loader
  PRIVATE transport_libcoro_ipc_dynamic_dll
          transport_streaming
          spsc_queue
          yas_common
          rpc::rpc
          ${CANOPY_LIBRARIES})

target_compile_options(canopy_libcoro_ipc_loader PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(canopy_libcoro_ipc_loader PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
set_property(TARGET canopy_libcoro_ipc_loader PROPERTY COMPILE_PDB_NAME canopy_libcoro_ipc_loader)
