#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

add_library(transport_c_abi STATIC src/dynamic_library_loader.cpp src/transport.cpp)

target_compile_definitions(transport_c_abi PRIVATE ${CANOPY_DEFINES})

target_include_directories(
  transport_c_abi
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include>"
         "$<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>"
         "$<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/c++/rpc/include>"
  PRIVATE ${CANOPY_INCLUDES})

target_link_libraries(transport_c_abi PRIVATE yas_common rpc_types_idl rpc::rpc ${CANOPY_LIBRARIES})

if(UNIX AND NOT APPLE)
  target_link_libraries(transport_c_abi PRIVATE dl)
endif()

target_compile_options(transport_c_abi PRIVATE ${CANOPY_COMPILE_OPTIONS} ${CANOPY_WARN_OK})
target_link_options(transport_c_abi PRIVATE ${CANOPY_LINK_EXE_OPTIONS})

set_property(TARGET transport_c_abi PROPERTY COMPILE_PDB_NAME transport_c_abi)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(transport_c_abi PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()

add_library(transport_c_abi_dll STATIC src/dll_transport.cpp)

target_compile_definitions(
  transport_c_abi_dll PRIVATE ${CANOPY_DEFINES} CANOPY_C_ABI_DLL_BUILDING)

target_include_directories(
  transport_c_abi_dll
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include>"
         "$<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>"
         "$<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/c++/rpc/include>"
  PRIVATE ${CANOPY_INCLUDES})

target_link_libraries(transport_c_abi_dll PRIVATE yas_common rpc_types_idl rpc::rpc ${CANOPY_LIBRARIES})

target_compile_options(
  transport_c_abi_dll
  PRIVATE ${CANOPY_COMPILE_OPTIONS}
          ${CANOPY_WARN_OK}
          $<$<CXX_COMPILER_ID:GNU,Clang>:-fvisibility=hidden>
          $<$<CXX_COMPILER_ID:GNU,Clang>:-fvisibility-inlines-hidden>)

target_link_options(transport_c_abi_dll PRIVATE ${CANOPY_LINK_EXE_OPTIONS})

set_property(TARGET transport_c_abi_dll PROPERTY COMPILE_PDB_NAME transport_c_abi_dll)

if(CANOPY_ENABLE_CLANG_TIDY)
  set_target_properties(transport_c_abi_dll PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()

add_library(transport_c_abi_test INTERFACE)
target_include_directories(transport_c_abi_test INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/tests>")
