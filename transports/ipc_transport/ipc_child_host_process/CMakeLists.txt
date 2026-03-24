if(NOT CANOPY_BUILD_COROUTINE)
  return()
endif()

add_executable(canopy_ipc_child_host_process src/main.cpp src/rpc_log.cpp)

target_compile_definitions(canopy_ipc_child_host_process PRIVATE ${CANOPY_DEFINES})

target_include_directories(canopy_ipc_child_host_process PRIVATE ${CANOPY_INCLUDES})

target_link_libraries(
  canopy_ipc_child_host_process
  PRIVATE transport_ipc_transport
          transport_libcoro_spsc_dll_host
          transport_streaming
          spsc_queue
          yas_common
          rpc::rpc
          ${CANOPY_LIBRARIES})

target_compile_options(canopy_ipc_child_host_process PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(canopy_ipc_child_host_process PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
set_property(TARGET canopy_ipc_child_host_process PROPERTY COMPILE_PDB_NAME canopy_ipc_child_host_process)
