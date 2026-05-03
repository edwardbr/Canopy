/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <canopy/fake_sgx/canopy_coroutine_startup_status.h>
#include <edl/canopy_coroutine_enclave.h>
#include <sgx_urts.h>

#include <cstddef>

extern "C"
{
    sgx_status_t canopy_coroutine_init_enclave(
        sgx_enclave_id_t enclave_id,
        int* retval,
        std::size_t req_sz,
        const char* req,
        void* host_to_enclave_queue,
        void* enclave_to_host_queue,
        canopy_coroutine_startup_status* startup_status,
        std::size_t resp_cap,
        char* resp,
        std::size_t* resp_sz);

    sgx_status_t canopy_coroutine_enter_thread(
        sgx_enclave_id_t enclave_id,
        int* retval,
        std::size_t req_sz,
        const char* req);
}
