/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <cstdint>

namespace rpc::sgx::coro::common
{
    constexpr std::uint32_t startup_status_abi_version = 1;

    enum class startup_state : std::uint32_t
    {
        pending = 0,
        workers_requested = 1,
        worker_ready = 2,
        runtime_ready = 3,
        failed = 5,
        shutting_down = 6,
        stopped = 7,
    };

    struct startup_status
    {
        std::uint32_t abi_version{startup_status_abi_version};
        std::uint32_t state{static_cast<std::uint32_t>(startup_state::pending)};
        std::int32_t error_code{0};
        std::uint32_t requested_workers{0};
        std::uint32_t attached_workers{0};
    };

    inline auto startup_load_u32(const std::uint32_t* value) noexcept -> std::uint32_t
    {
        return __atomic_load_n(value, __ATOMIC_ACQUIRE);
    }

    inline auto startup_load_i32(const std::int32_t* value) noexcept -> std::int32_t
    {
        return __atomic_load_n(value, __ATOMIC_ACQUIRE);
    }

    inline auto startup_store_u32(
        std::uint32_t* value,
        std::uint32_t new_value) noexcept -> void
    {
        __atomic_store_n(value, new_value, __ATOMIC_RELEASE);
    }

    inline auto startup_store_i32(
        std::int32_t* value,
        std::int32_t new_value) noexcept -> void
    {
        __atomic_store_n(value, new_value, __ATOMIC_RELEASE);
    }

    inline auto initialise_startup_status(startup_status& status) noexcept -> void
    {
        startup_store_u32(&status.abi_version, startup_status_abi_version);
        startup_store_u32(&status.state, static_cast<std::uint32_t>(startup_state::pending));
        startup_store_i32(&status.error_code, 0);
        startup_store_u32(&status.requested_workers, 0);
        startup_store_u32(&status.attached_workers, 0);
    }
}
