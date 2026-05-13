/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <edl/coroutine_enclave.h>

#include <cstdint>

namespace rpc::sgx::coro::common
{
    inline auto startup_load_error(const error_code* value) noexcept -> error_code
    {
        static_assert(sizeof(error_code) == sizeof(int));
        return __atomic_load_n(value, __ATOMIC_ACQUIRE);
    }

    inline auto startup_store_error(
        error_code* value,
        error_code new_value) noexcept -> void
    {
        static_assert(sizeof(error_code) == sizeof(int));
        __atomic_store_n(value, new_value, __ATOMIC_RELEASE);
    }

    inline auto startup_load_state(const rpc::sgx::coro::protocol::startup_state* value) noexcept
        -> rpc::sgx::coro::protocol::startup_state
    {
        using state_word = std::uint32_t;
        static_assert(sizeof(rpc::sgx::coro::protocol::startup_state) == sizeof(state_word));
        const auto loaded = __atomic_load_n(reinterpret_cast<const state_word*>(value), __ATOMIC_ACQUIRE);
        return static_cast<rpc::sgx::coro::protocol::startup_state>(loaded);
    }

    inline auto startup_store_state(
        rpc::sgx::coro::protocol::startup_state* value,
        rpc::sgx::coro::protocol::startup_state state) noexcept -> void
    {
        using state_word = std::uint32_t;
        static_assert(sizeof(rpc::sgx::coro::protocol::startup_state) == sizeof(state_word));
        const auto stored = static_cast<state_word>(state);
        __atomic_store_n(reinterpret_cast<state_word*>(value), stored, __ATOMIC_RELEASE);
    }

    inline auto initialise_startup_status(
        rpc::sgx::coro::protocol::startup_state& state,
        error_code& error) noexcept -> void
    {
        startup_store_state(&state, rpc::sgx::coro::protocol::startup_state::pending);
        startup_store_error(&error, 0);
    }
}
