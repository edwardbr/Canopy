/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <canopy/fake_sgx/canopy_coroutine_startup_status.h>
#include <edl/coroutine_enclave.h>
#include <sgx_error.h>

#include <cstddef>
#include <cstdint>

extern "C"
{
    int coroutine_init_enclave(
        std::size_t req_sz,
        const char* req,
        void* host_to_enclave_queue,
        void* enclave_to_host_queue,
        uint64_t ticks_per_millisecond,
        uint64_t initial_unix_epoch_milliseconds,
        canopy_coroutine_startup_status* startup_status,
        std::size_t resp_cap,
        char* resp,
        std::size_t* resp_sz);

    int coroutine_enter_thread(
        std::size_t req_sz,
        const char* req);
}
