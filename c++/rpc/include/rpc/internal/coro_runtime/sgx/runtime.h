/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <coro/concepts/awaitable.hpp>
#include <coro/net/io_status.hpp>

#include <type_traits>
#include <utility>

namespace rpc::coro::sgx
{
    namespace detail
    {
        template<
            ::coro::concepts::awaitable awaitable_type,
            typename return_type = typename ::coro::concepts::awaitable_traits<awaitable_type>::awaiter_return_type>
        auto sync_wait_task(awaitable_type&& awaitable) -> task<return_type>
        {
            if constexpr (std::is_void_v<return_type>)
            {
                co_await std::forward<awaitable_type>(awaitable);
                co_return;
            }
            else
            {
                co_return co_await std::forward<awaitable_type>(awaitable);
            }
        }
    }

    template<
        ::coro::concepts::awaitable awaitable_type,
        typename return_type = typename ::coro::concepts::awaitable_traits<awaitable_type>::awaiter_return_type>
    auto sync_wait(awaitable_type&& awaitable) -> return_type
    {
        auto wait_task = detail::sync_wait_task(std::forward<awaitable_type>(awaitable));
        while (wait_task.resume())
        {
        }

        if constexpr (std::is_void_v<return_type>)
        {
            wait_task.promise().result();
            return;
        }
        else if constexpr (std::is_reference_v<return_type>)
        {
            return wait_task.promise().result();
        }
        else if constexpr (std::is_move_constructible_v<std::remove_const_t<return_type>>)
        {
            return std::move(wait_task).promise().result();
        }
        else
        {
            return wait_task.promise().result();
        }
    }
} // namespace rpc::coro::sgx
