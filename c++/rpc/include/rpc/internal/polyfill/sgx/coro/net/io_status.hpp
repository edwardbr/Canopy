/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <string>
#include <string_view>

namespace coro::net
{
    struct io_status
    {
        enum class kind
        {
            ok,
            closed,
            connection_reset,
            connection_refused,
            timeout,

            would_block_or_try_again,
            polling_error,
            cancelled,

            udp_not_bound,
            message_too_big,

            native,

            unknown
        };

        kind type{};
        [[maybe_unused]] int native_code{};

        [[nodiscard]] auto is_ok() const -> bool { return type == kind::ok; }
        [[nodiscard]] auto is_timeout() const -> bool { return type == kind::timeout; }
        [[nodiscard]] auto is_closed() const -> bool { return type == kind::closed; }
        [[nodiscard]] auto would_block() const -> bool { return type == kind::would_block_or_try_again; }
        [[nodiscard]] auto try_again() const -> bool { return type == kind::would_block_or_try_again; }
        [[nodiscard]] auto is_native() const -> bool { return type == kind::native; }

        explicit operator bool() const { return is_ok(); }

        [[nodiscard]] auto message() const -> std::string;
    };

    inline auto to_string(io_status::kind kind) -> std::string_view;

    inline auto to_string(io_status::kind kind) -> std::string_view
    {
        switch (kind)
        {
        case io_status::kind::ok:
            return "ok";
        case io_status::kind::closed:
            return "closed";
        case io_status::kind::connection_reset:
            return "connection_reset";
        case io_status::kind::connection_refused:
            return "connection_refused";
        case io_status::kind::timeout:
            return "timeout";
        case io_status::kind::would_block_or_try_again:
            return "would_block_or_try_again";
        case io_status::kind::polling_error:
            return "polling_error";
        case io_status::kind::cancelled:
            return "cancelled";
        case io_status::kind::udp_not_bound:
            return "udp_not_bound";
        case io_status::kind::message_too_big:
            return "message_too_big";
        case io_status::kind::native:
            return "native";
        case io_status::kind::unknown:
            return "unknown";
        }

        return "unknown";
    }

    inline auto io_status::message() const -> std::string
    {
        return std::string(to_string(type));
    }

    inline auto make_io_status_from_native(int native_code) -> io_status
    {
        return io_status{.type = io_status::kind::native, .native_code = native_code};
    }
} // namespace coro::net
