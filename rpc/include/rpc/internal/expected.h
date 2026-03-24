/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

// Lightweight C++17 polyfill for std::expected<T,E> and std::unexpected<E> (C++23).
//
// Migration path to C++23:
//   template<typename T, typename E> using expected  = std::expected<T, E>;
//   template<typename E>             using unexpected = std::unexpected<E>;

#include <cassert>
#include <variant>
#include <utility>

namespace rpc
{
    // Mirrors std::unexpected<E>.  Wraps an error value so it can be assigned
    // to an expected<T,E> unambiguously.
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

    // Mirrors std::expected<T,E>.
    //
    // Preconditions (checked with assert in debug builds):
    //   value(), operator*, operator->  require has_value() == true
    //   error()                         requires has_value() == false
    template<typename T, typename E> class expected
    {
        std::variant<T, E> storage_;

    public:
        // Construct a successful result.
        constexpr expected(T val)
            : storage_(std::move(val))
        {
        }

        // Construct a failed result from an unexpected<E>.
        template<typename G>
        constexpr expected(unexpected<G> u)
            : storage_(std::move(u.error()))
        {
        }

        [[nodiscard]] constexpr bool has_value() const noexcept { return std::holds_alternative<T>(storage_); }
        constexpr explicit operator bool() const noexcept { return has_value(); }

        // Value access — undefined behaviour (asserted) if !has_value().
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

        // Error access — undefined behaviour (asserted) if has_value().
        constexpr E& error() & noexcept { return std::get<E>(storage_); }
        constexpr const E& error() const& noexcept { return std::get<E>(storage_); }
        constexpr E&& error() && noexcept { return std::get<E>(std::move(storage_)); }
    };

    // Specialisation for expected<void, E> — models an operation that either
    // succeeds (no value) or fails with an error.  Matches std::expected<void,E>.
    template<typename E> class expected<void, E>
    {
        std::variant<std::monostate, E> storage_;

    public:
        // Default construct = success.
        constexpr expected()
            : storage_(std::monostate{})
        {
        }

        // Construct a failed result from an unexpected<E>.
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

} // namespace rpc
