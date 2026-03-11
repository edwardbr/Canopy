// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#pragma once

#include <streaming/stream.h>

#include <arpa/inet.h>
#include <chrono>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace streaming
{
    class iouring_stream : public stream
    {
    public:
        iouring_stream(int fd, std::shared_ptr<coro::scheduler> scheduler)
            : fd_(fd)
            , scheduler_(std::move(scheduler))
        {
        }

        ~iouring_stream() override
        {
            if (fd_ != -1)
            {
                ::close(fd_);
            }
        }

        auto receive(std::span<char> buffer, std::chrono::milliseconds timeout = std::chrono::milliseconds{0})
            -> coro::task<std::pair<coro::net::io_status, std::span<char>>> override
        {
            if (closed_)
            {
                co_return {coro::net::io_status{coro::net::io_status::kind::native, EBADF}, {}};
            }

            auto deadline = std::chrono::steady_clock::now() + timeout;
            while (true)
            {
                ssize_t n = ::recv(fd_, buffer.data(), buffer.size(), MSG_DONTWAIT);

                if (n > 0)
                {
                    co_return {coro::net::io_status{coro::net::io_status::kind::ok, 0},
                        buffer.subspan(0, static_cast<size_t>(n))};
                }
                if (n == 0)
                {
                    closed_ = true;
                    co_return {coro::net::io_status{coro::net::io_status::kind::closed, 0}, {}};
                }

                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                {
                    co_return {coro::net::io_status{coro::net::io_status::kind::native, errno}, {}};
                }

                if (timeout <= std::chrono::milliseconds::zero())
                {
                    co_return {coro::net::io_status{coro::net::io_status::kind::timeout, 0}, {}};
                }

                auto now = std::chrono::steady_clock::now();
                if (now >= deadline)
                {
                    co_return {coro::net::io_status{coro::net::io_status::kind::timeout, 0}, {}};
                }

                auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
                co_await scheduler_->yield_for(std::min(remaining, std::chrono::milliseconds{1}));
            }
        }

        auto send(std::span<const char> buffer) -> coro::task<coro::net::io_status> override
        {
            if (closed_)
            {
                co_return coro::net::io_status{coro::net::io_status::kind::native, EBADF};
            }

            size_t total_sent = 0;
            while (total_sent < buffer.size())
            {
                ssize_t n = ::send(fd_, buffer.data() + total_sent, buffer.size() - total_sent, MSG_NOSIGNAL);

                if (n <= 0)
                {
                    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                    {
                        co_await scheduler_->yield_for(std::chrono::milliseconds{1});
                        continue;
                    }
                    closed_ = true;
                    co_return coro::net::io_status{
                        n < 0 ? coro::net::io_status::kind::native : coro::net::io_status::kind::closed, n < 0 ? errno : 0};
                }
                total_sent += static_cast<size_t>(n);
            }
            co_return coro::net::io_status{coro::net::io_status::kind::ok, 0};
        }

        bool is_closed() const override { return closed_; }

        void set_closed() override
        {
            closed_ = true;
            if (fd_ != -1)
            {
                ::shutdown(fd_, SHUT_RDWR);
            }
        }

        peer_info get_peer_info() const override { return {}; }

    private:
        int fd_ = -1;
        std::shared_ptr<coro::scheduler> scheduler_;
        bool closed_ = false;
    };

} // namespace streaming
