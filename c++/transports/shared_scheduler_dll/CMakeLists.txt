#
# transport_shared_scheduler_dll       — host-side (links into host process)
# transport_shared_scheduler_dll_runtime — DLL-side  (links into shared object)
#
# Both targets are only meaningful in coroutine builds.

if(NOT CANOPY_BUILD_COROUTINE)
  return()
endif()

include(CanopyCreateSharedSchedulerDll)

CanopyGenerate(
  shared_dll_config
  shared_scheduler_dll/config.idl
  ${CMAKE_CURRENT_SOURCE_DIR}/interfaces
  ${CMAKE_BINARY_DIR}/generated
  ""
  yas_json
  dependencies rpc_types_idl
  include_paths ${CMAKE_CURRENT_SOURCE_DIR}/interfaces ${CANOPY_INTERFACE_SOURCE_DIR}
  install_dir ${GENERATED_INSTALL_DIR})

# ---------------------------------------------------------------------------
# Host-side transport library
# ---------------------------------------------------------------------------
add_library(transport_shared_scheduler_dll STATIC src/transport.cpp src/factory.cpp)

target_compile_definitions(transport_shared_scheduler_dll PRIVATE ${CANOPY_DEFINES})

target_include_directories(
  transport_shared_scheduler_dll
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
         "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated/include>"
         "$<INSTALL_INTERFACE:include>"
  PRIVATE ${CANOPY_INCLUDES})

target_link_libraries(
  transport_shared_scheduler_dll
  PUBLIC transport_factory shared_dll_config_idl
  PRIVATE yas_common rpc_types_idl ${CANOPY_LIBRARIES})

if(UNIX AND NOT APPLE)
  target_link_libraries(transport_shared_scheduler_dll PRIVATE dl)
endif()

target_compile_options(transport_shared_scheduler_dll PRIVATE ${CANOPY_COMPILE_OPTIONS}
                                                                                ${CANOPY_WARN_OK})
target_link_options(transport_shared_scheduler_dll PRIVATE ${CANOPY_LINK_EXE_OPTIONS})

set_property(TARGET transport_shared_scheduler_dll
             PROPERTY COMPILE_PDB_NAME transport_shared_scheduler_dll)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(transport_shared_scheduler_dll PROPERTIES CXX_CLANG_TIDY
                                                                                    "${CLANG_TIDY_COMMAND}")
endif()

# ---------------------------------------------------------------------------
# DLL-side transport library
# ---------------------------------------------------------------------------
add_library(transport_shared_scheduler_dll_runtime STATIC src/dll_transport.cpp)

target_compile_definitions(transport_shared_scheduler_dll_runtime
                           PRIVATE ${CANOPY_DEFINES} CANOPY_SHARED_SCHEDULER_DLL_BUILDING)

target_include_directories(
  transport_shared_scheduler_dll_runtime
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include>"
  PRIVATE ${CANOPY_INCLUDES})

target_link_libraries(transport_shared_scheduler_dll_runtime PRIVATE yas_common rpc_types_idl
                                                                                   ${CANOPY_LIBRARIES})

target_compile_options(
  transport_shared_scheduler_dll_runtime
  PRIVATE ${CANOPY_COMPILE_OPTIONS}
          ${CANOPY_WARN_OK}
          $<$<CXX_COMPILER_ID:GNU,Clang>:-fvisibility=hidden>
          $<$<CXX_COMPILER_ID:GNU,Clang>:-fvisibility-inlines-hidden>
          $<$<CXX_COMPILER_ID:GNU>:-fno-gnu-unique>)

target_link_options(transport_shared_scheduler_dll_runtime PRIVATE ${CANOPY_LINK_EXE_OPTIONS})

set_property(TARGET transport_shared_scheduler_dll_runtime
             PROPERTY COMPILE_PDB_NAME transport_shared_scheduler_dll_runtime)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(transport_shared_scheduler_dll_runtime PROPERTIES CXX_CLANG_TIDY
                                                                                        "${CLANG_TIDY_COMMAND}")
endif()

# ---------------------------------------------------------------------------
# Test support: INTERFACE library that exposes the setup.h include path
# ---------------------------------------------------------------------------
add_library(transport_shared_scheduler_dll_test INTERFACE)
target_include_directories(transport_shared_scheduler_dll_test
                           INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/tests>")
