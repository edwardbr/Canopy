// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#pragma once

#ifdef __linux__

#  include <chrono>
#  include <atomic>
#  include <cstring>
#  include <cerrno>
#  include <memory>
#  include <netinet/in.h>
#  include <optional>
#  include <ostream>
#  include <fcntl.h>
#  include <sys/socket.h>
#  include <unistd.h>

#  include <coro/coro.hpp>

#  include <canopy/network_config/network_args.h>
#  include <streaming/io_uring/stream.h>
#  include <streaming/stream_acceptor.h>

namespace streaming::io_uring
{
    namespace debug
    {
        struct acceptor_diagnostics
        {
            std::atomic<uint64_t> init_calls{0};
            std::atomic<uint64_t> accept_calls{0};
            std::atomic<uint64_t> accept_successes{0};
            std::atomic<uint64_t> accept_eagain_loops{0};
            std::atomic<uint64_t> stop_calls{0};
        };

        inline auto acceptor_diag() -> acceptor_diagnostics&
        {
            static acceptor_diagnostics diag;
            return diag;
        }

        inline void reset_acceptor_diagnostics()
        {
            auto& d = acceptor_diag();
            d.init_calls.store(0, std::memory_order_relaxed);
            d.accept_calls.store(0, std::memory_order_relaxed);
            d.accept_successes.store(0, std::memory_order_relaxed);
            d.accept_eagain_loops.store(0, std::memory_order_relaxed);
            d.stop_calls.store(0, std::memory_order_relaxed);
        }

        inline void dump_acceptor_diagnostics(std::ostream& out)
        {
            auto& d = acceptor_diag();
            out << "io_uring acceptor diagnostics:"
                << " init_calls=" << d.init_calls.load(std::memory_order_relaxed)
                << " accept_calls=" << d.accept_calls.load(std::memory_order_relaxed)
                << " accept_successes=" << d.accept_successes.load(std::memory_order_relaxed)
                << " accept_eagain_loops=" << d.accept_eagain_loops.load(std::memory_order_relaxed)
                << " stop_calls=" << d.stop_calls.load(std::memory_order_relaxed)
                << '\n';
        }
    } // namespace debug

    class acceptor : public ::streaming::stream_acceptor
    {
    public:
        acceptor(canopy::network_config::ip_address address, uint16_t port)
            : address_(address)
            , port_(port)
        {
        }

        bool init(std::shared_ptr<coro::scheduler> scheduler) override
        {
            debug::acceptor_diag().init_calls.fetch_add(1, std::memory_order_relaxed);
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
            debug::acceptor_diag().accept_calls.fetch_add(1, std::memory_order_relaxed);
            while (!stop_requested_)
            {
                auto client_fd = ::accept(listen_fd_, nullptr, nullptr);
                if (client_fd >= 0)
                {
                    debug::acceptor_diag().accept_successes.fetch_add(1, std::memory_order_relaxed);
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
                debug::acceptor_diag().accept_eagain_loops.fetch_add(1, std::memory_order_relaxed);
                CO_AWAIT scheduler_->yield_for(std::chrono::milliseconds{1});
            }
            CO_RETURN std::nullopt;
        }

        void stop() override
        {
            debug::acceptor_diag().stop_calls.fetch_add(1, std::memory_order_relaxed);
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
