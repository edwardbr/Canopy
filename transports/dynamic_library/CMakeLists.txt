#
# transport_dynamic_library       — host-side library (links into the host process) transport_dynamic_library_dll   —
# DLL-side library  (links into the loaded shared object)
#

include(CanopyCreateDynamicLibrary)

# ---------------------------------------------------------------------------
# Host-side transport library
# ---------------------------------------------------------------------------
add_library(transport_dynamic_library STATIC src/transport.cpp)

target_compile_definitions(transport_dynamic_library PRIVATE ${CANOPY_DEFINES})

target_include_directories(
  transport_dynamic_library
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  PRIVATE ${CANOPY_INCLUDES})

target_link_libraries(transport_dynamic_library PRIVATE yas_common rpc_types_idl ${CANOPY_LIBRARIES})

# Link dl on Linux for dlopen/dlclose/dlsym
if(UNIX AND NOT APPLE)
  target_link_libraries(transport_dynamic_library PRIVATE dl)
endif()

target_compile_options(transport_dynamic_library PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(transport_dynamic_library PRIVATE ${CANOPY_LINK_EXE_OPTIONS})

set_property(TARGET transport_dynamic_library PROPERTY COMPILE_PDB_NAME transport_dynamic_library)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(transport_dynamic_library PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()

# ---------------------------------------------------------------------------
# DLL-side transport library
#
# Shared objects that want to participate in the dynamic_library transport link this library.  It provides the
# canopy_dll_destroy / canopy_dll_send / ... entry points.  The DLL author only needs to provide canopy_dll_init (using
# the init_child_zone<> helper from dll_transport.h).
#
# Build with -fvisibility=hidden to keep implementation symbols private; the CANOPY_DLL_EXPORT attribute on the
# canopy_dll_* symbols overrides that for the handful of entry points the host must be able to find via dlsym.
# ---------------------------------------------------------------------------
add_library(transport_dynamic_library_dll STATIC src/dll_transport.cpp)

target_compile_definitions(
  transport_dynamic_library_dll PRIVATE ${CANOPY_DEFINES} CANOPY_DLL_BUILDING # activates __declspec(dllexport) on
                                                                              # Windows
)

target_include_directories(
  transport_dynamic_library_dll
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  PRIVATE ${CANOPY_INCLUDES})

target_link_libraries(transport_dynamic_library_dll PRIVATE yas_common rpc_types_idl ${CANOPY_LIBRARIES})

target_compile_options(
  transport_dynamic_library_dll
  PRIVATE ${CANOPY_COMPILE_OPTIONS}
          ${CANOPY_WARN_OK}
          # Hide all symbols by default; canopy_dll_* use __attribute__((visibility("default")))
          $<$<CXX_COMPILER_ID:GNU,Clang>:-fvisibility=hidden>
          $<$<CXX_COMPILER_ID:GNU,Clang>:-fvisibility-inlines-hidden>)

target_link_options(transport_dynamic_library_dll PRIVATE ${CANOPY_LINK_EXE_OPTIONS})

set_property(TARGET transport_dynamic_library_dll PROPERTY COMPILE_PDB_NAME transport_dynamic_library_dll)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(transport_dynamic_library_dll PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()

# ---------------------------------------------------------------------------
# Test support: INTERFACE library that exposes the setup.h include path
# ---------------------------------------------------------------------------
add_library(transport_dynamic_library_test INTERFACE)
target_include_directories(transport_dynamic_library_test
                           INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/tests>")
