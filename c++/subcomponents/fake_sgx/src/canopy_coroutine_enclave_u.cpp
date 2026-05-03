/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <canopy/fake_sgx/runtime.h>
#include <edl/canopy_coroutine_enclave.h>
#include <transports/sgx_coroutine/common/shared_queue.h>
#include <transports/sgx_coroutine/common/startup_status.h>
#include <untrusted/canopy_coroutine_enclave_u.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <vector>

namespace
{
    using trusted_init_fn = int (*)(
        std::size_t,
        const char*,
        void*,
        void*,
        canopy_coroutine_startup_status*,
        std::size_t,
        char*,
        std::size_t*);

    using trusted_enter_thread_fn = int (*)(
        std::size_t,
        const char*);

    auto copy_to_enclave_buffer(
        const char* data,
        std::size_t size) -> std::vector<char>
    {
        std::vector<char> result(size);
        if (data && size > 0)
            std::memcpy(result.data(), data, size);
        return result;
    }
}

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
        std::size_t* resp_sz)
    {
        if (!retval || (!req && req_sz > 0) || (!resp && resp_cap > 0) || !resp_sz)
            return SGX_ERROR_INVALID_PARAMETER;

        *retval = 0;
        *resp_sz = 0;

        canopy::fake_sgx::scoped_enclave_call call(enclave_id);
        if (!call.ok())
            return call.status();

        auto* raw_symbol = call.symbol("canopy_coroutine_init_enclave");
        if (!raw_symbol)
            return SGX_ERROR_UNEXPECTED;

        auto* trusted_init = reinterpret_cast<trusted_init_fn>(raw_symbol);
        auto enclave_req = copy_to_enclave_buffer(req, req_sz);
        std::vector<char> enclave_resp(resp_cap);
        std::size_t enclave_resp_sz = 0;

        call.add_inside(enclave_req.data(), enclave_req.size());
        call.add_inside(enclave_resp.data(), enclave_resp.size());
        call.add_inside(&enclave_resp_sz, sizeof(enclave_resp_sz));
        call.add_outside(host_to_enclave_queue, sizeof(rpc::sgx::coro::common::queue_type));
        call.add_outside(enclave_to_host_queue, sizeof(rpc::sgx::coro::common::queue_type));
        call.add_outside(startup_status, sizeof(rpc::sgx::coro::common::startup_status));

        *retval = trusted_init(
            enclave_req.size(),
            enclave_req.empty() ? nullptr : enclave_req.data(),
            host_to_enclave_queue,
            enclave_to_host_queue,
            startup_status,
            enclave_resp.size(),
            enclave_resp.empty() ? nullptr : enclave_resp.data(),
            &enclave_resp_sz);

        *resp_sz = enclave_resp_sz;
        if (enclave_resp_sz > resp_cap)
            return SGX_SUCCESS;

        if (resp && enclave_resp_sz > 0)
            std::memcpy(resp, enclave_resp.data(), enclave_resp_sz);
        return SGX_SUCCESS;
    }

    sgx_status_t canopy_coroutine_enter_thread(
        sgx_enclave_id_t enclave_id,
        int* retval,
        std::size_t req_sz,
        const char* req)
    {
        if (!retval || (!req && req_sz > 0))
            return SGX_ERROR_INVALID_PARAMETER;

        *retval = 0;

        canopy::fake_sgx::scoped_enclave_call call(enclave_id);
        if (!call.ok())
            return call.status();

        auto* raw_symbol = call.symbol("canopy_coroutine_enter_thread");
        if (!raw_symbol)
            return SGX_ERROR_UNEXPECTED;

        auto* trusted_enter_thread = reinterpret_cast<trusted_enter_thread_fn>(raw_symbol);
        auto enclave_req = copy_to_enclave_buffer(req, req_sz);
        call.add_inside(enclave_req.data(), enclave_req.size());

        *retval = trusted_enter_thread(enclave_req.size(), enclave_req.empty() ? nullptr : enclave_req.data());
        return SGX_SUCCESS;
    }
}
