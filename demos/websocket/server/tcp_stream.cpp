// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

// tcp_stream.cpp - Plain TCP stream implementation
#include "tcp_stream.h"

namespace websocket_demo
{
    namespace v1
    {
        tcp_stream::tcp_stream(coro::net::tcp::client&& client)
            : client_(std::move(client))
        {
        }

        auto tcp_stream::poll(coro::poll_op op, std::chrono::milliseconds timeout) -> coro::task<coro::poll_status>
        {
            return client_.poll(op, timeout);
        }

        auto tcp_stream::recv(std::span<char> buffer) -> std::pair<coro::net::recv_status, std::span<char>>
        {
            auto result = client_.recv(buffer);
            if (result.first == coro::net::recv_status::closed)
            {
                closed_ = true;
            }
            return result;
        }

        auto tcp_stream::send(std::span<const char> buffer) -> std::pair<coro::net::send_status, std::span<const char>>
        {
            if (closed_)
            {
                return {coro::net::send_status::closed, buffer};
            }
            auto result = client_.send(buffer);
            if (result.first != coro::net::send_status::ok && result.first != coro::net::send_status::would_block)
            {
                closed_ = true;
            }
            return result;
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
