/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <canopy/fake_sgx/runtime.h>
#include <secure_coroutine_module/secure_coroutine_module.h>
#include <rpc/rpc.h>
#include <streaming/spsc_queue/stream.h>
#include <untrusted/canopy_coroutine_enclave_u.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

namespace
{
    using trusted_init_fn = void (*)(
        std::size_t,
        const char*,
        uint64_t,
        uint64_t);

    using trusted_enter_thread_fn = int (*)(
        std::size_t,
        const char*);

    struct ecall_byte_buffer
    {
        std::unique_ptr<char[]> bytes;
        std::size_t size = 0;

        [[nodiscard]] auto data() noexcept -> char* { return bytes.get(); }
        [[nodiscard]] auto data() const noexcept -> const char* { return bytes.get(); }
        [[nodiscard]] auto empty() const noexcept -> bool { return size == 0; }
    };

    auto copy_to_enclave_buffer(
        const char* data,
        std::size_t size) -> ecall_byte_buffer
    {
        ecall_byte_buffer result{size > 0 ? std::make_unique<char[]>(size) : nullptr, size};
        if (data && size > 0)
            std::memcpy(result.data(), data, size);
        return result;
    }

    void* pointer_from_request_address(uint64_t address) noexcept
    {
        return reinterpret_cast<void*>(static_cast<std::uintptr_t>(address));
    }

    rpc::encoding normalise_request_encoding(uint64_t request_encoding) noexcept
    {
        if (request_encoding == 0)
            return rpc::encoding::yas_binary;
        return static_cast<rpc::encoding>(request_encoding);
    }
}

extern "C"
{
    sgx_status_t coroutine_init_enclave(
        sgx_enclave_id_t enclave_id,
        std::size_t req_sz,
        const char* req,
        uint64_t request_encoding,
        uint64_t request_type_id)
    {
        if (!req && req_sz > 0)
            return SGX_ERROR_INVALID_PARAMETER;

        canopy::fake_sgx::scoped_enclave_call call(enclave_id);
        if (!call.ok())
            return call.status();

        auto* raw_symbol = call.symbol("coroutine_init_enclave");
        if (!raw_symbol)
            return SGX_ERROR_UNEXPECTED;

        auto* trusted_init = reinterpret_cast<trusted_init_fn>(raw_symbol);
        auto enclave_req = copy_to_enclave_buffer(req, req_sz);

        call.add_inside(enclave_req.data(), enclave_req.size);
        if (request_type_id == rpc::id<rpc::v4::secure_coroutine_module::init_request>::get(rpc::get_version()))
        {
            rpc::v4::secure_coroutine_module::init_request init_request{};
            auto decode_error = rpc::deserialise(
                normalise_request_encoding(request_encoding),
                rpc::byte_span{reinterpret_cast<const uint8_t*>(enclave_req.data()), enclave_req.size},
                init_request);
            if (decode_error.empty())
            {
                call.add_outside(
                    pointer_from_request_address(init_request.parent_to_runtime_queue_ptr),
                    sizeof(streaming::spsc_queue::queue_type));
                call.add_outside(
                    pointer_from_request_address(init_request.runtime_to_parent_queue_ptr),
                    sizeof(streaming::spsc_queue::queue_type));
                if (init_request.state)
                    call.add_outside(init_request.state, sizeof(*init_request.state));
                if (init_request.error)
                    call.add_outside(init_request.error, sizeof(*init_request.error));
            }
        }

        trusted_init(
            enclave_req.size, enclave_req.empty() ? nullptr : enclave_req.data(), request_encoding, request_type_id);
        return SGX_SUCCESS;
    }

    sgx_status_t coroutine_enter_thread(
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

        auto* raw_symbol = call.symbol("coroutine_enter_thread");
        if (!raw_symbol)
            return SGX_ERROR_UNEXPECTED;

        auto* trusted_enter_thread = reinterpret_cast<trusted_enter_thread_fn>(raw_symbol);
        auto enclave_req = copy_to_enclave_buffer(req, req_sz);
        call.add_inside(enclave_req.data(), enclave_req.size);

        *retval = trusted_enter_thread(enclave_req.size, enclave_req.empty() ? nullptr : enclave_req.data());
        return SGX_SUCCESS;
    }
}
