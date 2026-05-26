// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#include <streaming/tcp/socket.h>

#include <errno.h>
#include <sys/socket.h>
#include <utility>

#ifndef CANOPY_BUILD_COROUTINE
#  include <fcntl.h>
#  include <poll.h>
#  include <unistd.h>
#endif

namespace streaming::tcp
{
#ifdef CANOPY_BUILD_COROUTINE
    namespace
    {
        rpc::io_status status_ok() noexcept
        {
            rpc::io_status s;
            s.type = rpc::io_status::kind::ok;
            return s;
        }
    }

    socket::socket(
        coro::net::tcp::client&& client,
        std::shared_ptr<rpc::executor> executor)
        : client_(std::move(client))
        , executor_(std::move(executor))
    {
    }

    socket::socket(socket&& other) noexcept = default;
    socket& socket::operator=(socket&& other) noexcept = default;
    socket::~socket() = default;

    auto socket::read_some(
        rpc::mutable_byte_span buffer,
        std::chrono::milliseconds timeout) -> CORO_TASK(::streaming::receive_result)
    {
        auto [status, s] = co_await client_.read_some(buffer, timeout);
        co_return ::streaming::receive_result{status, rpc::mutable_byte_span(s.data(), s.size())};
    }

    auto socket::wait_writable(std::chrono::milliseconds /*timeout*/) -> CORO_TASK(rpc::io_status)
    {
        // Yield to the scheduler — matches the original
        // scheduler_->schedule() yield-and-retry behaviour of stream::send.
        // We deliberately do NOT use libcoro's epoll-based write-readiness
        // wait: the same fd is already registered for reads, and a second
        // EPOLL_CTL_ADD would fail with EEXIST.
        co_await executor_->schedule();
        co_return status_ok();
    }

    int socket::native_handle() const noexcept
    {
        return client_.socket().native_handle();
    }

    void socket::shutdown() noexcept
    {
        client_.socket().shutdown();
    }

    void socket::close() noexcept
    {
        client_.socket().close();
    }

    bool socket::is_open() const noexcept
    {
        return client_.socket().native_handle() >= 0;
    }

#else // blocking

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
        rpc::io_status status_timeout() noexcept
        {
            rpc::io_status s;
            s.type = rpc::io_status::kind::timeout;
            return s;
        }
        rpc::io_status status_would_block() noexcept
        {
            rpc::io_status s;
            s.type = rpc::io_status::kind::would_block_or_try_again;
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

    socket::socket(int fd)
        : fd_(fd)
    {
        if (fd_ >= 0)
        {
            int flags = ::fcntl(fd_, F_GETFL, 0);
            if (flags >= 0)
                ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
        }
    }

    socket::socket(socket&& other) noexcept
        : fd_(std::exchange(
              other.fd_,
              -1))
    {
    }

    socket& socket::operator=(socket&& other) noexcept
    {
        if (this != &other)
        {
            close();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    socket::~socket()
    {
        close();
    }

    auto socket::read_some(
        rpc::mutable_byte_span buffer,
        std::chrono::milliseconds timeout) -> ::streaming::receive_result
    {
        if (fd_ < 0)
            return {status_closed(), buffer.subspan(0, 0)};

        // poll first when the caller supplied a positive timeout. timeout == 0
        // means "block indefinitely" to match common stream-receive defaults.
        int t_ms = timeout.count() > 0 ? static_cast<int>(timeout.count()) : -1;
        pollfd pfd{};
        pfd.fd = fd_;
        pfd.events = POLLIN;
        int r = ::poll(&pfd, 1, t_ms);
        if (r == 0)
            return {status_timeout(), buffer.subspan(0, 0)};
        if (r < 0)
            return {status_native(errno), buffer.subspan(0, 0)};
        if (pfd.revents & POLLERR)
            return {status_native(0), buffer.subspan(0, 0)};

        ssize_t n = ::recv(fd_, buffer.data(), buffer.size(), 0);
        if (n == 0)
            return {status_closed(), buffer.subspan(0, 0)};
        if (n < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return {status_would_block(), buffer.subspan(0, 0)};
            if (errno == ECONNRESET)
            {
                rpc::io_status s;
                s.type = rpc::io_status::kind::connection_reset;
                return {s, buffer.subspan(0, 0)};
            }
            return {status_native(errno), buffer.subspan(0, 0)};
        }
        return {status_ok(), buffer.subspan(0, static_cast<size_t>(n))};
    }

    auto socket::wait_writable(std::chrono::milliseconds timeout) -> rpc::io_status
    {
        if (fd_ < 0)
            return status_closed();
        pollfd pfd{};
        pfd.fd = fd_;
        pfd.events = POLLOUT;
        int t_ms = timeout.count() > 0 ? static_cast<int>(timeout.count()) : -1;
        int r = ::poll(&pfd, 1, t_ms);
        if (r == 0)
            return status_timeout();
        if (r < 0)
            return status_native(errno);
        if (pfd.revents & POLLERR)
            return status_native(0);
        if (pfd.revents & POLLHUP)
            return status_closed();
        return status_ok();
    }

    int socket::native_handle() const noexcept
    {
        return fd_;
    }

    void socket::shutdown() noexcept
    {
        if (fd_ >= 0)
            ::shutdown(fd_, SHUT_RDWR);
    }

    void socket::close() noexcept
    {
        if (fd_ >= 0)
        {
            ::close(fd_);
            fd_ = -1;
        }
    }

    bool socket::is_open() const noexcept
    {
        return fd_ >= 0;
    }

#endif
} // namespace streaming::tcp
