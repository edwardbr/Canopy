// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

// tcp_stream.cpp - Plain TCP stream implementation
#include <streaming/tcp_stream.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <cstring>

namespace streaming
{
    tcp_stream::tcp_stream(coro::net::tcp::client&& client, std::shared_ptr<coro::scheduler> scheduler)
        : client_(std::move(client))
        , scheduler_(std::move(scheduler))
    {
    }

    auto tcp_stream::receive(std::span<char> buffer, std::chrono::milliseconds timeout)
        -> coro::task<std::pair<coro::net::io_status, std::span<char>>>
    {
        auto [status, span] = co_await client_.read_some(buffer, timeout);
        if (status.is_closed())
            closed_ = true;
        co_return {status, span};
    }

    auto tcp_stream::send(std::span<const char> buffer) -> coro::task<coro::net::io_status>
    {
        while (!buffer.empty())
        {
            auto [status, remaining] = co_await client_.write_some(buffer);
            if (!status.is_ok())
            {
                closed_ = true;
                co_return status;
            }
            buffer = remaining;
        }
        co_return coro::net::io_status{coro::net::io_status::kind::ok};
    }

    bool tcp_stream::is_closed() const
    {
        return closed_;
    }

    void tcp_stream::set_closed()
    {
        closed_ = true;
        if (!socket_closed_)
        {
            socket_closed_ = true;
            client_.socket().shutdown();
            client_.socket().close();
        }
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

} // namespace streaming
