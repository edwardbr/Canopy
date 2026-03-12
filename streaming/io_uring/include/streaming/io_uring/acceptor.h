// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#pragma once

#ifdef __linux__

#include <chrono>
#include <cstring>
#include <cerrno>
#include <memory>
#include <netinet/in.h>
#include <optional>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <coro/coro.hpp>

#include <canopy/network_config/network_args.h>
#include <streaming/io_uring/stream.h>
#include <streaming/stream_acceptor.h>

namespace streaming::io_uring
{
    class acceptor : public ::streaming::stream_acceptor
    {
    public:
        acceptor(canopy::network_config::ip_address address, uint16_t port)
            : address_(std::move(address))
            , port_(port)
        {
        }

        bool init(std::shared_ptr<coro::scheduler> scheduler) override
        {
            scheduler_ = std::move(scheduler);

            listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
            if (listen_fd_ < 0)
            {
                RPC_ERROR("io_uring::acceptor socket failed: {}", errno);
                return false;
            }

            if (!configure_socket(listen_fd_))
            {
                RPC_ERROR("io_uring::acceptor configure_socket failed: {}", errno);
                ::close(listen_fd_);
                listen_fd_ = -1;
                return false;
            }

            int opt = 1;
            if (::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
            {
                RPC_ERROR("io_uring::acceptor setsockopt(SO_REUSEADDR) failed: {}", errno);
                ::close(listen_fd_);
                listen_fd_ = -1;
                return false;
            }

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port_);
            std::memcpy(&addr.sin_addr, address_.data(), 4);

            if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
            {
                RPC_ERROR("io_uring::acceptor bind failed on {}.{}.{}.{}:{}: {}",
                    static_cast<int>(address_[0]),
                    static_cast<int>(address_[1]),
                    static_cast<int>(address_[2]),
                    static_cast<int>(address_[3]),
                    port_,
                    errno);
                ::close(listen_fd_);
                listen_fd_ = -1;
                return false;
            }

            if (::listen(listen_fd_, SOMAXCONN) < 0)
            {
                RPC_ERROR("io_uring::acceptor listen failed: {}", errno);
                ::close(listen_fd_);
                listen_fd_ = -1;
                return false;
            }

            stop_requested_ = false;
            return true;
        }

        CORO_TASK(std::optional<std::shared_ptr<::streaming::stream>>) accept() override
        {
            while (!stop_requested_)
            {
                auto client_fd = ::accept(listen_fd_, nullptr, nullptr);
                if (client_fd >= 0)
                {
                    if (!configure_socket(client_fd))
                    {
                        RPC_ERROR("io_uring::acceptor configure accepted socket failed: {}", errno);
                        ::close(client_fd);
                        break;
                    }

                    CO_RETURN stream::create_from_accepted_socket(client_fd, scheduler_);
                }
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                    break;
                CO_AWAIT scheduler_->yield_for(std::chrono::milliseconds{1});
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
        static auto configure_socket(int fd) -> bool
        {
            int status_flags = ::fcntl(fd, F_GETFL, 0);
            if (status_flags < 0 || ::fcntl(fd, F_SETFL, status_flags | O_NONBLOCK) < 0)
            {
                return false;
            }

            int descriptor_flags = ::fcntl(fd, F_GETFD, 0);
            if (descriptor_flags < 0 || ::fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) < 0)
            {
                return false;
            }

            return true;
        }

        canopy::network_config::ip_address address_;
        uint16_t port_;
        int listen_fd_ = -1;
        bool stop_requested_ = false;
        std::shared_ptr<coro::scheduler> scheduler_;
    };
} // namespace streaming::io_uring

#endif // __linux__
