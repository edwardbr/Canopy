#
# transport_libcoro_dynamic_library       — host-side (links into host process) transport_libcoro_dynamic_library_dll —
# DLL-side  (links into shared object)
#
# Both targets are only meaningful in coroutine builds.

if(NOT CANOPY_BUILD_COROUTINE)
  return()
endif()

include(CanopyCreateLibcoroDynamicLibrary)

# ---------------------------------------------------------------------------
# Host-side transport library
# ---------------------------------------------------------------------------
add_library(transport_libcoro_dynamic_library STATIC src/transport.cpp)

target_compile_definitions(transport_libcoro_dynamic_library PRIVATE ${CANOPY_DEFINES})

target_include_directories(
  transport_libcoro_dynamic_library
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  PRIVATE ${CANOPY_INCLUDES})

target_link_libraries(transport_libcoro_dynamic_library PRIVATE yas_common rpc_types_idl ${CANOPY_LIBRARIES})

if(UNIX AND NOT APPLE)
  target_link_libraries(transport_libcoro_dynamic_library PRIVATE dl)
endif()

target_compile_options(transport_libcoro_dynamic_library PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(transport_libcoro_dynamic_library PRIVATE ${CANOPY_LINK_EXE_OPTIONS})

set_property(TARGET transport_libcoro_dynamic_library PROPERTY COMPILE_PDB_NAME transport_libcoro_dynamic_library)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(transport_libcoro_dynamic_library PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()

# ---------------------------------------------------------------------------
# DLL-side transport library
# ---------------------------------------------------------------------------
add_library(transport_libcoro_dynamic_library_dll STATIC src/dll_transport.cpp)

target_compile_definitions(transport_libcoro_dynamic_library_dll PRIVATE ${CANOPY_DEFINES} CANOPY_LIBCORO_DLL_BUILDING)

target_include_directories(
  transport_libcoro_dynamic_library_dll
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  PRIVATE ${CANOPY_INCLUDES})

target_link_libraries(transport_libcoro_dynamic_library_dll PRIVATE yas_common rpc_types_idl ${CANOPY_LIBRARIES})

target_compile_options(
  transport_libcoro_dynamic_library_dll
  PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK} $<$<CXX_COMPILER_ID:GNU,Clang>:-fvisibility=hidden>
          $<$<CXX_COMPILER_ID:GNU,Clang>:-fvisibility-inlines-hidden>)

target_link_options(transport_libcoro_dynamic_library_dll PRIVATE ${CANOPY_LINK_EXE_OPTIONS})

set_property(TARGET transport_libcoro_dynamic_library_dll PROPERTY COMPILE_PDB_NAME
                                                                   transport_libcoro_dynamic_library_dll)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(transport_libcoro_dynamic_library_dll PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()

# ---------------------------------------------------------------------------
# Test support: INTERFACE library that exposes the setup.h include path
# ---------------------------------------------------------------------------
add_library(transport_libcoro_dynamic_library_test INTERFACE)
target_include_directories(transport_libcoro_dynamic_library_test
                           INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/tests>")
