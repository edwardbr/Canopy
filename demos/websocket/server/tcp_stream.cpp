// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

// tcp_stream.cpp - Plain TCP stream implementation
#include "tcp_stream.h"
#include <sys/socket.h>
#include <errno.h>

namespace websocket_demo
{
    namespace v1
    {
        tcp_stream::tcp_stream(coro::net::tcp::client&& client, std::shared_ptr<coro::scheduler> scheduler)
            : client_(std::move(client))
            , scheduler_(std::move(scheduler))
        {
        }

        auto tcp_stream::poll(coro::poll_op op, std::chrono::milliseconds timeout) -> coro::task<coro::poll_status>
        {
            co_return co_await scheduler_->poll(client_.socket(), op, timeout);
        }

        auto tcp_stream::recv(std::span<char> buffer, std::chrono::milliseconds timeout)
            -> coro::task<std::pair<coro::net::io_status, std::span<char>>>
        {
            auto [status, span] = co_await client_.read_some(buffer, timeout);
            if (status.is_closed())
                closed_ = true;
            co_return {status, span};
        }

        auto tcp_stream::send(std::span<const char> buffer) -> std::pair<coro::net::io_status, std::span<const char>>
        {
            if (closed_)
            {
                return {coro::net::io_status{coro::net::io_status::kind::closed}, buffer};
            }
            auto bytes = ::send(client_.socket().native_handle(), buffer.data(), buffer.size(), MSG_NOSIGNAL);
            if (bytes >= 0)
            {
                return {coro::net::io_status{coro::net::io_status::kind::ok},
                    std::span<const char>{buffer.data() + bytes, buffer.size() - static_cast<size_t>(bytes)}};
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                return {coro::net::io_status{coro::net::io_status::kind::would_block_or_try_again}, buffer};
            }
            closed_ = true;
            return {coro::net::io_status{coro::net::io_status::kind::native, errno}, buffer};
        }

        bool tcp_stream::is_closed() const
        {
            return closed_;
        }

        void tcp_stream::set_closed()
        {
            closed_ = true;
        }

        coro::net::tcp::client& tcp_stream::client()
        {
            return client_;
        }

    } // namespace websocket_demo
}
