#
# transport_libcoro_ipc_dynamic_dll       - host-side helpers / loader transport_libcoro_ipc_dynamic_dll_dll   -
# DLL-side bootstrap
#

if(NOT CANOPY_BUILD_COROUTINE)
  return()
endif()

include(CanopyCreateLibcoroIpcDynamicLibrary)

add_library(transport_libcoro_ipc_dynamic_dll STATIC src/transport.cpp src/loaded_library.cpp)

target_compile_definitions(transport_libcoro_ipc_dynamic_dll PRIVATE ${CANOPY_DEFINES})

target_include_directories(
  transport_libcoro_ipc_dynamic_dll
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
         "$<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/transports/streaming/include>"
         "$<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/streaming/core/include>"
         "$<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/streaming/spsc_queue/include>"
         "$<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/subcomponents/spsc_queue/include>"
         "$<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/subcomponents/network_config/include>"
         "$<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/submodules/args>"
         ${CANOPY_INCLUDES})

target_link_libraries(
  transport_libcoro_ipc_dynamic_dll
  PRIVATE transport_streaming
          yas_common
          rpc_types_idl
          spsc_queue
          ${CANOPY_LIBRARIES})

if(UNIX AND NOT APPLE)
  target_link_libraries(transport_libcoro_ipc_dynamic_dll PRIVATE dl)
endif()

target_compile_options(transport_libcoro_ipc_dynamic_dll PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(transport_libcoro_ipc_dynamic_dll PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
set_property(TARGET transport_libcoro_ipc_dynamic_dll PROPERTY COMPILE_PDB_NAME transport_libcoro_ipc_dynamic_dll)

add_library(transport_libcoro_ipc_dynamic_dll_dll STATIC src/dll_transport.cpp)

target_compile_definitions(transport_libcoro_ipc_dynamic_dll_dll PRIVATE ${CANOPY_DEFINES}
                                                                         CANOPY_LIBCORO_IPC_DLL_BUILDING)

target_include_directories(
  transport_libcoro_ipc_dynamic_dll_dll
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
         "$<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/transports/streaming/include>"
         "$<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/streaming/core/include>"
         "$<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/streaming/spsc_queue/include>"
         "$<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/subcomponents/spsc_queue/include>"
         "$<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/subcomponents/network_config/include>"
         "$<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/submodules/args>"
         ${CANOPY_INCLUDES})

target_link_libraries(
  transport_libcoro_ipc_dynamic_dll_dll
  PRIVATE transport_streaming
          yas_common
          rpc_types_idl
          spsc_queue
          ${CANOPY_LIBRARIES})

target_compile_options(
  transport_libcoro_ipc_dynamic_dll_dll
  PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK} $<$<CXX_COMPILER_ID:GNU,Clang>:-fvisibility=hidden>
          $<$<CXX_COMPILER_ID:GNU,Clang>:-fvisibility-inlines-hidden>)

target_link_options(transport_libcoro_ipc_dynamic_dll_dll PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
set_property(TARGET transport_libcoro_ipc_dynamic_dll_dll PROPERTY COMPILE_PDB_NAME
                                                                   transport_libcoro_ipc_dynamic_dll_dll)

add_library(transport_libcoro_ipc_dynamic_dll_test INTERFACE)
target_include_directories(transport_libcoro_ipc_dynamic_dll_test
                           INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/tests>")
