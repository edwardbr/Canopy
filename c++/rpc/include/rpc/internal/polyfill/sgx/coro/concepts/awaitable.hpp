/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <coroutine>
#include <type_traits>
#include <utility>

namespace coro::concepts
{
    namespace detail
    {
        template<typename awaitable_type> decltype(auto) get_awaiter(awaitable_type&& awaitable)
        {
            if constexpr (requires { std::forward<awaitable_type>(awaitable).operator co_await(); })
            {
                return std::forward<awaitable_type>(awaitable).operator co_await();
            }
            else if constexpr (requires { operator co_await(std::forward<awaitable_type>(awaitable)); })
            {
                return operator co_await(std::forward<awaitable_type>(awaitable));
            }
            else
            {
                return std::forward<awaitable_type>(awaitable);
            }
        }
    } // namespace detail

    template<typename awaiter_type>
    concept awaiter = requires(awaiter_type&& awaiter, std::coroutine_handle<> awaiting_coroutine) {
        awaiter.await_ready();
        requires std::is_convertible_v<decltype(awaiter.await_ready()), bool>;
        awaiter.await_suspend(awaiting_coroutine);
        awaiter.await_resume();
    };

    template<typename awaitable_type>
    concept awaitable = awaiter<decltype(detail::get_awaiter(std::declval<awaitable_type>()))>;

    template<typename awaitable_type> struct awaitable_traits
    {
        using awaiter_type = decltype(detail::get_awaiter(std::declval<awaitable_type>()));
        using awaiter_return_type = decltype(std::declval<awaiter_type>().await_resume());
    };
} // namespace coro::concepts
