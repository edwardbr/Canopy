#pragma once

#ifdef __linux__

#include <chrono>
#include <cstring>
#include <memory>
#include <netinet/in.h>
#include <optional>
#include <sys/socket.h>
#include <unistd.h>

#include <coro/coro.hpp>

#include <canopy/network_config/network_args.h>
#include <streaming/io_uring_stream.h>
#include <streaming/stream_acceptor.h>

namespace streaming
{
    // Backward-compatible acceptor for existing streaming_iouring tests.
    // This implementation is fully non-blocking: accept readiness is polled via
    // the scheduler and accepted sockets are created with SOCK_NONBLOCK.
    class iouring_acceptor : public stream_acceptor
    {
    public:
        iouring_acceptor(canopy::network_config::ip_address address, uint16_t port)
            : address_(std::move(address))
            , port_(port)
        {
        }

        bool init(std::shared_ptr<coro::scheduler> scheduler) override
        {
            scheduler_ = std::move(scheduler);

            listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            if (listen_fd_ < 0)
                return false;

            int opt = 1;
            ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port_);
            std::memcpy(&addr.sin_addr, address_.data(), 4);

            if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
            {
                ::close(listen_fd_);
                listen_fd_ = -1;
                return false;
            }

            if (::listen(listen_fd_, SOMAXCONN) < 0)
            {
                ::close(listen_fd_);
                listen_fd_ = -1;
                return false;
            }

            stop_requested_ = false;
            return true;
        }

        CORO_TASK(std::optional<std::shared_ptr<stream>>) accept() override
        {
            while (!stop_requested_)
            {
                auto poll_status
                    = CO_AWAIT scheduler_->poll(listen_fd_, coro::poll_op::read, std::chrono::milliseconds{10});
                if (poll_status == coro::poll_status::timeout)
                    continue;
                if (poll_status != coro::poll_status::read)
                    break;

                auto client_fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
                if (client_fd >= 0)
                    CO_RETURN std::make_shared<iouring_stream>(client_fd, scheduler_);
            }
            CO_RETURN std::nullopt;
        }

        void stop() override
        {
            stop_requested_ = true;
            if (listen_fd_ != -1)
            {
                ::close(listen_fd_);
                listen_fd_ = -1;
            }
        }

    private:
        canopy::network_config::ip_address address_;
        uint16_t port_;
        int listen_fd_ = -1;
        bool stop_requested_ = false;
        std::shared_ptr<coro::scheduler> scheduler_;
    };
} // namespace streaming

#endif // __linux__
