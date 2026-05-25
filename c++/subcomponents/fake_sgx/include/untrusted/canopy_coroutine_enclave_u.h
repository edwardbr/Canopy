/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <secure_coroutine_module/secure_coroutine_module.h>
#include <sgx_urts.h>

#include <cstddef>
#include <cstdint>

extern "C"
{
    sgx_status_t coroutine_init_enclave(
        sgx_enclave_id_t enclave_id,
        std::size_t req_sz,
        const char* req,
        uint64_t request_encoding,
        uint64_t request_type_id);

    sgx_status_t coroutine_enter_thread(
        sgx_enclave_id_t enclave_id,
        int* retval,
        std::size_t req_sz,
        const char* req);
}
