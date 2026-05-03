/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <cstdint>

using sgx_status_t = std::int32_t;

constexpr sgx_status_t SGX_SUCCESS = 0;
constexpr sgx_status_t SGX_ERROR_UNEXPECTED = 1;
constexpr sgx_status_t SGX_ERROR_INVALID_PARAMETER = 2;
constexpr sgx_status_t SGX_ERROR_OUT_OF_MEMORY = 3;
constexpr sgx_status_t SGX_ERROR_ENCLAVE_LOST = 4;
constexpr sgx_status_t SGX_ERROR_INVALID_ENCLAVE = 5;
