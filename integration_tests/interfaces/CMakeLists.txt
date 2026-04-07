#[[
   Copyright (c) 2026 Edward Boggis-Rolfe
   All rights reserved.
]]

cmake_minimum_required(VERSION 3.24)

CanopyGenerate(
  fuzz_test
  fuzz_test/fuzz_test.idl
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated
  ""
  yas_binary
  yas_compressed_binary
  yas_json
  protocol_buffers
  mock fuzz_test/fuzz_test_mock.h
  include_paths ${CMAKE_CURRENT_SOURCE_DIR}/.)
