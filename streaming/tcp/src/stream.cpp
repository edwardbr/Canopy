// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

// tcp stream implementation
#include <streaming/tcp/stream.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <errno.h>
#include <cstring>

namespace streaming::tcp
{
    stream::stream(coro::net::tcp::client&& client, std::shared_ptr<coro::scheduler> scheduler)
        : client_(std::move(client))
        , scheduler_(std::move(scheduler))
    {
        // Disable Nagle's algorithm so small RPC packets are sent immediately.
        // Without this, Nagle + delayed-ACK interaction causes ~40-80ms stalls per
        // round-trip for payloads smaller than one TCP segment.
        int flag = 1;
        ::setsockopt(
            client_.socket().native_handle(), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&flag), sizeof(flag));
    }

    auto stream::receive(rpc::mutable_byte_span buffer, std::chrono::milliseconds timeout)
        -> coro::task<std::pair<coro::net::io_status, rpc::mutable_byte_span>>
    {
        auto [status, s] = co_await client_.read_some(buffer, timeout);
        if (status.is_closed())
            closed_ = true;
        co_return {status, rpc::mutable_byte_span(s.data(), s.size())};
    }

    auto stream::send(rpc::byte_span buffer) -> coro::task<coro::net::io_status>
    {
        // Use direct ::send() syscalls rather than libcoro's poll-based write path.
        // libcoro's write_some() uses epoll (EPOLL_CTL_ADD) to wait for write-readiness,
        // but the same socket fd is already registered in epoll by receive_consumer_loop for
        // reads. A second EPOLL_CTL_ADD for the same fd fails with EEXIST, which would leave
        // send_producer_loop permanently stuck. Bypassing libcoro's poll for writes avoids
        // this concurrent-registration conflict entirely. When the send buffer is full
        // (EAGAIN/EWOULDBLOCK), we yield via schedule() and retry.
        while (!buffer.empty())
        {
            ssize_t bytes_sent
                = ::send(client_.socket().native_handle(), buffer.data(), buffer.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
            if (bytes_sent > 0)
            {
                buffer = buffer.subspan(bytes_sent);
            }
            else if (bytes_sent == 0)
            {
                closed_ = true;
                co_return coro::net::io_status{.type = coro::net::io_status::kind::closed};
            }
            else
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    co_await scheduler_->schedule();
                    continue;
                }
                closed_ = true;
                co_return coro::net::io_status{.type = coro::net::io_status::kind::native, .native_code = errno};
            }
        }
        co_return coro::net::io_status{.type = coro::net::io_status::kind::ok};
    }

    bool stream::is_closed() const
    {
        return closed_;
    }

    void stream::set_closed()
    {
        closed_ = true;
        if (!socket_closed_)
        {
            socket_closed_ = true;
            client_.socket().shutdown();
            client_.socket().close();
        }
    }

    auto stream::client() -> coro::net::tcp::client&
    {
        return client_;
    }

    auto stream::get_peer_info() const -> peer_info
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

} // namespace streaming::tcp
