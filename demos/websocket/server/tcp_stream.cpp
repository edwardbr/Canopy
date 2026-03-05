// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

// tcp_stream.cpp - Plain TCP stream implementation
#include "tcp_stream.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <cstring>

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

        peer_info tcp_stream::get_peer_info() const
        {
            peer_info info{};
            sockaddr_storage ss{};
            socklen_t len = sizeof(ss);
            if (::getpeername(client_.socket().native_handle(), reinterpret_cast<sockaddr*>(&ss), &len) != 0)
                return info;

            if (ss.ss_family == AF_INET)
            {
                const auto* sin = reinterpret_cast<const sockaddr_in*>(&ss);
                const uint32_t addr = ntohl(sin->sin_addr.s_addr);
                info.addr[0] = static_cast<uint8_t>(addr >> 24);
                info.addr[1] = static_cast<uint8_t>(addr >> 16);
                info.addr[2] = static_cast<uint8_t>(addr >> 8);
                info.addr[3] = static_cast<uint8_t>(addr);
                info.family = canopy::network_config::ip_address_family::ipv4;
                info.port = ntohs(sin->sin_port);
            }
            else if (ss.ss_family == AF_INET6)
            {
                const auto* sin6 = reinterpret_cast<const sockaddr_in6*>(&ss);
                std::memcpy(info.addr.data(), sin6->sin6_addr.s6_addr, 16);
                info.family = canopy::network_config::ip_address_family::ipv6;
                info.port = ntohs(sin6->sin6_port);
            }
            return info;
        }

    } // namespace websocket_demo
}
