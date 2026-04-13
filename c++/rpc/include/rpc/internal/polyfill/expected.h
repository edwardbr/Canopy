/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#if defined(__has_include)
#  if __has_include(<expected>) && ((defined(_MSVC_LANG) && _MSVC_LANG >= 202302L) || __cplusplus >= 202302L)
#    include <expected>
#    if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202202L
#      define RPC_HAS_STD_EXPECTED 1
#    endif
#  endif
#endif

#ifndef RPC_HAS_STD_EXPECTED
#  include <cassert>
#  include <utility>
#  include <variant>
#endif

namespace rpc
{
#ifdef RPC_HAS_STD_EXPECTED
    template<typename E> using unexpected = std::unexpected<E>;
    template<typename T, typename E> using expected = std::expected<T, E>;
#else
    template<typename E> class unexpected
    {
    public:
        unexpected() = delete;
        constexpr explicit unexpected(const E& e)
            : error_(e)
        {
        }
        constexpr explicit unexpected(E&& e)
            : error_(std::move(e))
        {
        }

        constexpr E& error() & noexcept { return error_; }
        constexpr const E& error() const& noexcept { return error_; }
        constexpr E&& error() && noexcept { return std::move(error_); }

    private:
        E error_;
    };

    template<typename T, typename E> class expected
    {
        std::variant<T, E> storage_;

    public:
        constexpr expected(T val)
            : storage_(std::move(val))
        {
        }

        template<typename G>
        constexpr expected(unexpected<G> u)
            : storage_(std::move(u.error()))
        {
        }

        [[nodiscard]] constexpr bool has_value() const noexcept { return std::holds_alternative<T>(storage_); }
        constexpr explicit operator bool() const noexcept { return has_value(); }

        constexpr T& operator*() & noexcept { return std::get<T>(storage_); }
        constexpr const T& operator*() const& noexcept { return std::get<T>(storage_); }
        constexpr T* operator->() noexcept { return &std::get<T>(storage_); }
        constexpr const T* operator->() const noexcept { return &std::get<T>(storage_); }

        constexpr T& value() &
        {
            assert(has_value());
            return std::get<T>(storage_);
        }
        constexpr const T& value() const&
        {
            assert(has_value());
            return std::get<T>(storage_);
        }
        constexpr T&& value() &&
        {
            assert(has_value());
            return std::get<T>(std::move(storage_));
        }

        constexpr E& error() & noexcept { return std::get<E>(storage_); }
        constexpr const E& error() const& noexcept { return std::get<E>(storage_); }
        constexpr E&& error() && noexcept { return std::get<E>(std::move(storage_)); }
    };

    template<typename E> class expected<void, E>
    {
        std::variant<std::monostate, E> storage_;

    public:
        constexpr expected()
            : storage_(std::monostate{})
        {
        }

        template<typename G>
        constexpr expected(unexpected<G> u)
            : storage_(std::move(u.error()))
        {
        }

        [[nodiscard]] constexpr bool has_value() const noexcept
        {
            return std::holds_alternative<std::monostate>(storage_);
        }
        constexpr explicit operator bool() const noexcept { return has_value(); }

        constexpr void value() const { assert(has_value()); }

        constexpr E& error() & noexcept { return std::get<E>(storage_); }
        constexpr const E& error() const& noexcept { return std::get<E>(storage_); }
        constexpr E&& error() && noexcept { return std::get<E>(std::move(storage_)); }
    };
#endif
}
