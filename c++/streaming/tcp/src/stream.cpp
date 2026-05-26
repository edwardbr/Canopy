// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

// tcp stream implementation — identical source in coroutine and blocking
// builds. Mode-specific I/O behaviour lives in streaming::tcp::socket.

#include <streaming/tcp/stream.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <errno.h>
#include <cstring>

namespace streaming::tcp
{
    namespace
    {
        rpc::io_status status_ok() noexcept
        {
            rpc::io_status s;
            s.type = rpc::io_status::kind::ok;
            return s;
        }
        rpc::io_status status_closed() noexcept
        {
            rpc::io_status s;
            s.type = rpc::io_status::kind::closed;
            return s;
        }
        rpc::io_status status_native(int err) noexcept
        {
            rpc::io_status s;
            s.type = rpc::io_status::kind::native;
            s.native_code = err;
            return s;
        }
    }

    stream::stream(socket sock)
        : socket_(std::move(sock))
    {
        // Disable Nagle's algorithm so small RPC packets are sent immediately.
        // Without this, Nagle + delayed-ACK interaction causes ~40-80ms stalls
        // per round-trip for payloads smaller than one TCP segment.
        int flag = 1;
        ::setsockopt(
            socket_.native_handle(), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&flag), sizeof(flag));
    }

#ifdef CANOPY_BUILD_COROUTINE
    stream::stream(coro::net::tcp::client&& client, std::shared_ptr<rpc::executor> executor)
        : stream(socket(std::move(client), std::move(executor)))
    {
    }
#endif

    auto stream::receive(
        rpc::mutable_byte_span buffer,
        std::chrono::milliseconds timeout) -> CORO_TASK(::streaming::receive_result)
    {
        auto result = CO_AWAIT socket_.read_some(buffer, timeout);
        if (result.first.is_closed())
            closed_ = true;
        CO_RETURN result;
    }

    auto stream::send(rpc::byte_span buffer) -> CORO_TASK(rpc::io_status)
    {
        // Use direct ::send() syscalls rather than libcoro's poll-based write
        // path. libcoro's write_some() uses epoll (EPOLL_CTL_ADD) to wait for
        // write-readiness, but the same socket fd is already registered in
        // epoll by receive_consumer_loop for reads. A second EPOLL_CTL_ADD
        // for the same fd fails with EEXIST, which would leave
        // send_producer_loop permanently stuck. Bypassing libcoro's poll
        // for writes avoids this concurrent-registration conflict entirely.
        // On EAGAIN/EWOULDBLOCK we delegate to socket_.wait_writable: in
        // coroutine mode that yields to the scheduler (cooperative retry);
        // in blocking mode it does poll(POLLOUT) so we don't burn a core.
        while (!buffer.empty())
        {
            ssize_t bytes_sent
                = ::send(socket_.native_handle(), buffer.data(), buffer.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
            if (bytes_sent > 0)
            {
                buffer = buffer.subspan(bytes_sent);
            }
            else if (bytes_sent == 0)
            {
                closed_ = true;
                CO_RETURN status_closed();
            }
            else
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    RPC_WARNING(
                        "tcp::stream::send EAGAIN: {} bytes remaining, fd={}",
                        buffer.size(),
                        socket_.native_handle());
                    auto wstatus = CO_AWAIT socket_.wait_writable(std::chrono::seconds(30));
                    if (!wstatus.is_ok())
                    {
                        closed_ = true;
                        CO_RETURN wstatus;
                    }
                    continue;
                }
                closed_ = true;
                CO_RETURN status_native(errno);
            }
        }
        CO_RETURN status_ok();
    }

    bool stream::is_closed() const
    {
        return closed_;
    }

    auto stream::set_closed() -> CORO_TASK(void)
    {
        closed_.store(true, std::memory_order_release);
        // CAS the close-once flag so concurrent set_closed()/send()/receive()
        // calls only invoke ::shutdown + ::close on the underlying socket
        // once. Without the CAS, two threads could both pass the !flag
        // check and double-close.
        bool expected = false;
        if (socket_closed_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        {
            socket_.shutdown();
            socket_.close();
        }
        CO_RETURN;
    }

    auto stream::get_peer_info() const -> peer_info
    {
        peer_info info{};
        sockaddr_storage ss{};
        socklen_t len = sizeof(ss);
        if (::getpeername(socket_.native_handle(), reinterpret_cast<sockaddr*>(&ss), &len) != 0)
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
