/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <sgx_error.h>
#include <sgx_urts.h>

#include <cstddef>
#include <cstdint>

namespace canopy::fake_sgx
{
    enum class memory_kind : std::uint8_t
    {
        inside,
        outside,
    };

    class scoped_enclave_call
    {
    public:
        explicit scoped_enclave_call(sgx_enclave_id_t enclave_id) noexcept;
        scoped_enclave_call(const scoped_enclave_call&) = delete;
        scoped_enclave_call(scoped_enclave_call&&) = delete;
        auto operator=(const scoped_enclave_call&) -> scoped_enclave_call& = delete;
        auto operator=(scoped_enclave_call&&) -> scoped_enclave_call& = delete;
        ~scoped_enclave_call();

        [[nodiscard]] auto status() const noexcept -> sgx_status_t;
        [[nodiscard]] auto ok() const noexcept -> bool;
        [[nodiscard]] auto symbol(const char* name) const noexcept -> void*;

        auto add_inside(
            const void* address,
            std::size_t size) noexcept -> void;
        auto add_outside(
            const void* address,
            std::size_t size) noexcept -> void;

    private:
        sgx_enclave_id_t enclave_id_{0};
        sgx_enclave_id_t previous_enclave_id_{0};
        std::uint64_t call_id_{0};
        sgx_status_t status_{SGX_SUCCESS};
    };
}
