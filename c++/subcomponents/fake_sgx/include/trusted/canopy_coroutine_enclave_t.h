/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <edl/coroutine_enclave.h>
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
