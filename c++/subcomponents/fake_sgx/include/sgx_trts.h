/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <cstddef>

#include <sgx_error.h>

extern "C"
{
    int sgx_is_within_enclave(
        const void* address,
        std::size_t size);

    int sgx_is_outside_enclave(
        const void* address,
        std::size_t size);

    sgx_status_t sgx_read_rand(
        unsigned char* rand,
        std::size_t length_in_bytes);
}
