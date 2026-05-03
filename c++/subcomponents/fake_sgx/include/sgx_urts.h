/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <sgx_error.h>

#include <cstdint>

using sgx_enclave_id_t = std::uint64_t;
using sgx_launch_token_t = std::uint8_t[1024];

#ifndef SGX_DEBUG_FLAG
#  define SGX_DEBUG_FLAG 1
#endif

extern "C"
{
    sgx_status_t sgx_create_enclave(
        const char* file_name,
        int debug,
        sgx_launch_token_t* launch_token,
        int* launch_token_updated,
        sgx_enclave_id_t* enclave_id,
        void* misc_attr);

    sgx_status_t sgx_destroy_enclave(sgx_enclave_id_t enclave_id);
}
