if(NOT CANOPY_BUILD_COROUTINE)
  return()
endif()

CanopyGenerate(
  ipc_spsc_config
  ipc_spsc/config.idl
  ${CMAKE_CURRENT_SOURCE_DIR}/interfaces
  ${CMAKE_BINARY_DIR}/generated
  ""
  yas_json
  dependencies rpc_types_idl
  include_paths ${CMAKE_CURRENT_SOURCE_DIR}/interfaces ${CANOPY_INTERFACE_SOURCE_DIR}
  install_dir ${GENERATED_INSTALL_DIR})

add_library(transport_ipc_spsc INTERFACE)
add_library(transport_ipc_spsc_impl STATIC src/bootstrap.cpp src/transport.cpp src/factory.cpp)
add_library(transport_ipc_spsc_dll_host STATIC src/loaded_library.cpp src/queue_transport.cpp)
add_library(transport_ipc_spsc_dll_runtime STATIC src/dll_transport.cpp)

target_compile_definitions(transport_ipc_spsc_impl PRIVATE ${CANOPY_DEFINES})
target_compile_definitions(transport_ipc_spsc_dll_host PRIVATE ${CANOPY_DEFINES})
target_compile_definitions(transport_ipc_spsc_dll_runtime PRIVATE ${CANOPY_DEFINES} CANOPY_IPC_SPSC_DLL_BUILDING)

target_include_directories(
  transport_ipc_spsc_impl
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
         "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated/include>"
         "$<BUILD_INTERFACE:${CANOPY_CPP_SOURCE_DIR}/transports/streaming/include>"
         "$<BUILD_INTERFACE:${CANOPY_CPP_SOURCE_DIR}/streaming/core/include>"
         "$<BUILD_INTERFACE:${CANOPY_CPP_SOURCE_DIR}/streaming/spsc_queue/include>"
         "$<BUILD_INTERFACE:${CANOPY_CPP_SOURCE_DIR}/subcomponents/spsc_queue/include>"
         "$<INSTALL_INTERFACE:include>"
         ${CANOPY_INCLUDES})

target_link_libraries(
  transport_ipc_spsc_impl
  PUBLIC transport_factory
         transport_streaming
         streaming_spsc_queue
         ipc_spsc_config_idl
         yas_common
         spsc_queue
         args::args
         rpc::rpc
         ${CANOPY_LIBRARIES})

target_compile_options(transport_ipc_spsc_impl PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(transport_ipc_spsc_impl PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
set_property(TARGET transport_ipc_spsc_impl PROPERTY COMPILE_PDB_NAME transport_ipc_spsc_impl)

target_include_directories(
  transport_ipc_spsc_dll_host
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
         "$<BUILD_INTERFACE:${CANOPY_CPP_SOURCE_DIR}/transports/streaming/include>"
         "$<BUILD_INTERFACE:${CANOPY_CPP_SOURCE_DIR}/streaming/core/include>"
         "$<BUILD_INTERFACE:${CANOPY_CPP_SOURCE_DIR}/streaming/spsc_queue/include>"
         "$<BUILD_INTERFACE:${CANOPY_CPP_SOURCE_DIR}/subcomponents/spsc_queue/include>"
         "$<INSTALL_INTERFACE:include>"
         ${CANOPY_INCLUDES})
target_link_libraries(
  transport_ipc_spsc_dll_host
  PUBLIC transport_streaming
         streaming_spsc_queue
         yas_common
         spsc_queue
         rpc::rpc
         ${CANOPY_LIBRARIES})
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(transport_ipc_spsc_dll_host PUBLIC dl)
endif()
target_compile_options(transport_ipc_spsc_dll_host PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(transport_ipc_spsc_dll_host PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
set_property(TARGET transport_ipc_spsc_dll_host PROPERTY COMPILE_PDB_NAME transport_ipc_spsc_dll_host)

target_include_directories(
  transport_ipc_spsc_dll_runtime
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
         "$<BUILD_INTERFACE:${CANOPY_CPP_SOURCE_DIR}/transports/streaming/include>"
         "$<BUILD_INTERFACE:${CANOPY_CPP_SOURCE_DIR}/streaming/core/include>"
         "$<BUILD_INTERFACE:${CANOPY_CPP_SOURCE_DIR}/streaming/spsc_queue/include>"
         "$<BUILD_INTERFACE:${CANOPY_CPP_SOURCE_DIR}/subcomponents/spsc_queue/include>"
         "$<INSTALL_INTERFACE:include>"
         ${CANOPY_INCLUDES})
target_link_libraries(
  transport_ipc_spsc_dll_runtime
  PUBLIC transport_streaming
         streaming_spsc_queue
         yas_common
         spsc_queue
         rpc::rpc
         ${CANOPY_LIBRARIES})
target_compile_options(transport_ipc_spsc_dll_runtime PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(transport_ipc_spsc_dll_runtime PRIVATE ${CANOPY_LINK_EXE_OPTIONS})
set_property(TARGET transport_ipc_spsc_dll_runtime PROPERTY COMPILE_PDB_NAME transport_ipc_spsc_dll_runtime)

target_link_libraries(transport_ipc_spsc INTERFACE transport_ipc_spsc_impl)
target_include_directories(transport_ipc_spsc INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
                                                        "$<INSTALL_INTERFACE:include>")

add_library(transport_ipc_spsc_test INTERFACE)
target_include_directories(transport_ipc_spsc_test INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/tests>")
target_link_libraries(transport_ipc_spsc_test INTERFACE transport_ipc_spsc)

add_library(transport_ipc_spsc_dll_host_test INTERFACE)
target_include_directories(transport_ipc_spsc_dll_host_test
                           INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/tests>")
target_link_libraries(transport_ipc_spsc_dll_host_test INTERFACE transport_ipc_spsc_dll_host)

# Generic sidecar helpers. The built-in executable hosts a configured DLL; a direct executable still belongs in the app
# or test that owns its service interfaces.
add_subdirectory(sidecar_process)

if(TARGET canopy_ipc_spsc_sidecar_process)
  target_compile_definitions(
    transport_ipc_spsc_impl
    PRIVATE CANOPY_IPC_SPSC_SIDECAR_PROCESS_PATH="$<TARGET_FILE:canopy_ipc_spsc_sidecar_process>")
endif()

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(transport_ipc_spsc_impl transport_ipc_spsc_dll_host transport_ipc_spsc_dll_runtime
                        PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()
