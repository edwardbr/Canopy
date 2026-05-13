/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <canopy/fake_sgx/canopy_coroutine_startup_status.h>
#include <edl/coroutine_enclave.h>
#include <sgx_urts.h>

#include <cstddef>
#include <cstdint>

extern "C"
{
    sgx_status_t coroutine_init_enclave(
        sgx_enclave_id_t enclave_id,
        int* retval,
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

    sgx_status_t coroutine_enter_thread(
        sgx_enclave_id_t enclave_id,
        int* retval,
        std::size_t req_sz,
        const char* req);
}
