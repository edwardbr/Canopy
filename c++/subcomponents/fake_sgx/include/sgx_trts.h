/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <cstddef>

extern "C"
{
    int sgx_is_within_enclave(
        const void* address,
        std::size_t size);

    int sgx_is_outside_enclave(
        const void* address,
        std::size_t size);
}
