/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <secure_coroutine_module/secure_coroutine_module.h>
#include <sgx_error.h>

#include <cstddef>
#include <cstdint>

extern "C"
{
    void coroutine_init_enclave(
        std::size_t req_sz,
        const char* req,
        uint64_t request_encoding,
        uint64_t request_type_id);

    int coroutine_enter_thread(
        std::size_t req_sz,
        const char* req);
}
