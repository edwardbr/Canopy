if(NOT CANOPY_BUILD_COROUTINE)
  return()
endif()

add_library(transport_ipc_child_process INTERFACE)
target_include_directories(
  transport_ipc_child_process
  INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
            "$<BUILD_INTERFACE:${CANOPY_SOURCE_DIR}/transports/streaming/include>"
            "$<BUILD_INTERFACE:${CANOPY_SOURCE_DIR}/streaming/core/include>"
            "$<BUILD_INTERFACE:${CANOPY_SOURCE_DIR}/streaming/spsc_queue/include>"
            "$<BUILD_INTERFACE:${CANOPY_SOURCE_DIR}/subcomponents/spsc_queue/include>"
            ${CANOPY_INCLUDES})

add_executable(canopy_ipc_child_process src/main.cpp src/rpc_log.cpp)

target_compile_definitions(canopy_ipc_child_process PRIVATE ${CANOPY_DEFINES})

target_include_directories(canopy_ipc_child_process PRIVATE ${CANOPY_INCLUDES}
                                                            "${CANOPY_SOURCE_DIR}/transports/local/include")

target_link_libraries(
  canopy_ipc_child_process
  PRIVATE transport_ipc_transport
          transport_streaming
          transport_local
          common_tests
          example_idl
          example_import_idl
          example_shared_idl
          spsc_queue
          yas_common
          rpc::rpc
          ${CANOPY_LIBRARIES})

target_compile_options(canopy_ipc_child_process PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(canopy_ipc_child_process PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
set_property(TARGET canopy_ipc_child_process PROPERTY COMPILE_PDB_NAME canopy_ipc_child_process)
