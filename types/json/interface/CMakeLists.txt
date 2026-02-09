cmake_minimum_required(VERSION 3.24)

# make json dom objects available to other idls you dont actually need to link against this as it automagically appears

CanopyGenerate(
  json
  json/json.idl
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated
  ""
  yas_binary
  yas_compressed_binary
  yas_json
  # protocol_buffers // I have not given love to proto buffers yet
  include_paths ${CMAKE_CURRENT_SOURCE_DIR}/.
  install_dir include/secretarium/idls)

if(WIN32)
  # sneek in a win32 specific dependancy
  target_link_libraries(json_idl_enclave PUBLIC mpark_variant)
endif()

# Suppress generator bug warning (misleading indentation in generated code)
target_compile_options(json_idl PRIVATE -Wno-misleading-indentation)
target_include_directories(json_idl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../include)
