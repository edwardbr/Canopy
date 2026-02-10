cmake_minimum_required(VERSION 3.24)

CanopyGenerate(
  secret_llama
  secret_llama/secret_llama.idl
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated
  ""
  yas_binary
  yas_compressed_binary
  yas_json
  protocol_buffers
  install_dir include/secret_llama/idls
  mock secret_llama/secret_llama_mock.h
  dependencies json_idl
  defines ${BUILD_TEST_FLAG}
  include_paths ${CMAKE_CURRENT_SOURCE_DIR}/. ${FileSystem_DIR}/../include)
