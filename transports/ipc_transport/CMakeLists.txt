if(NOT CANOPY_BUILD_COROUTINE)
  return()
endif()

add_library(transport_ipc_transport INTERFACE)
add_library(transport_ipc_transport_impl STATIC src/bootstrap.cpp src/transport.cpp)

target_compile_definitions(transport_ipc_transport_impl PRIVATE ${CANOPY_DEFINES})

target_include_directories(
  transport_ipc_transport_impl
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
         "$<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/transports/streaming/include>"
         "$<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/streaming/core/include>"
         "$<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/streaming/spsc_queue/include>"
         "$<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/subcomponents/spsc_queue/include>"
         ${CANOPY_INCLUDES})

target_link_libraries(
  transport_ipc_transport_impl
  PUBLIC transport_streaming
         transport_libcoro_spsc_dynamic_dll
         yas_common
         spsc_queue
         args::args
         rpc::rpc
         ${CANOPY_LIBRARIES})

target_compile_options(transport_ipc_transport_impl PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(transport_ipc_transport_impl PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
set_property(TARGET transport_ipc_transport_impl PROPERTY COMPILE_PDB_NAME transport_ipc_transport_impl)

target_link_libraries(transport_ipc_transport INTERFACE transport_ipc_transport_impl)
target_include_directories(transport_ipc_transport INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")

add_library(transport_ipc_transport_test INTERFACE)
target_include_directories(transport_ipc_transport_test
                           INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/tests>")

add_subdirectory(ipc_child_process)
add_subdirectory(ipc_child_host_process)
