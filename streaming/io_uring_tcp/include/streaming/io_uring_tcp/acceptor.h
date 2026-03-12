/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#ifdef __linux__

#include <atomic>
#include <chrono>
#include <cstring>
#include <liburing.h>
#include <memory>
#include <mutex>
#include <optional>
#include <sys/socket.h>
#include <unistd.h>

#include <coro/coro.hpp>
#include <coro/net/socket_address.hpp>
#include <coro/net/tcp/server.hpp>

#include <streaming/io_uring/stream.h>
#include <streaming/stream_acceptor.h>

namespace streaming::io_uring_tcp
{
    class acceptor : public ::streaming::stream_acceptor
    {
    public:
        acceptor(const coro::net::socket_address& endpoint, coro::net::tcp::server::options opts = {})
            : endpoint_(endpoint)
            , opts_(std::move(opts))
        {
        }

        ~acceptor() override
        {
            stop();
            teardown_ring();
        }

        bool init(std::shared_ptr<coro::scheduler> scheduler) override
        {
            scheduler_ = std::move(scheduler);
            stop_.store(false, std::memory_order_release);

            listen_fd_ = ::socket(static_cast<int>(endpoint_.domain()), SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            if (listen_fd_ < 0)
            {
                return false;
            }

            int opt = 1;
            if (::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
            {
                stop();
                return false;
            }

            if (::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0)
            {
                stop();
                return false;
            }

            auto [sockaddr, socklen] = endpoint_.data();
            if (::bind(listen_fd_, sockaddr, socklen) < 0)
            {
                stop();
                return false;
            }

            if (::listen(listen_fd_, opts_.backlog) < 0)
            {
                stop();
                return false;
            }

            if (!setup_ring())
            {
                stop();
                teardown_ring();
                return false;
            }

            return true;
        }

        CORO_TASK(std::optional<std::shared_ptr<::streaming::stream>>) accept() override
        {
            while (!stop_.load(std::memory_order_acquire))
            {
                sockaddr_storage client_storage{};
                socklen_t client_len = sizeof(client_storage);

                if (!submit_accept(client_storage, client_len))
                {
                    break;
                }

                while (!stop_.load(std::memory_order_acquire))
                {
                    auto completion = poll_accept_completion();
                    if (!completion.has_value())
                    {
                        CO_AWAIT scheduler_->yield_for(std::chrono::milliseconds{1});
                        continue;
                    }

                    if (*completion >= 0)
                    {
                        CO_RETURN ::streaming::io_uring::stream::create_from_accepted_socket(*completion, scheduler_);
                    }

                    int native_error = -*completion;
                    if (native_error == ECANCELED && stop_.load(std::memory_order_acquire))
                    {
                        CO_RETURN std::nullopt;
                    }

                    if (native_error == EINTR)
                    {
                        break;
                    }

                    if (!stop_.load(std::memory_order_acquire))
                    {
                        RPC_ERROR("io_uring_tcp::acceptor: accept error {}", native_error);
                    }
                    CO_RETURN std::nullopt;
                }
            }

            CO_RETURN std::nullopt;
        }

        void stop() override
        {
            stop_.store(true, std::memory_order_release);

            cancel_pending_accept();

            if (listen_fd_ != -1)
            {
                ::shutdown(listen_fd_, SHUT_RDWR);
                ::close(listen_fd_);
                listen_fd_ = -1;
            }
        }

    private:
        static constexpr uint64_t accept_user_data = 1;

        bool setup_ring()
        {
            std::lock_guard<std::mutex> lock(ring_mutex_);
            if (ring_ready_)
            {
                return true;
            }

            if (io_uring_queue_init(8, &ring_, 0) < 0)
            {
                return false;
            }

            ring_ready_ = true;
            return true;
        }

        void teardown_ring()
        {
            std::lock_guard<std::mutex> lock(ring_mutex_);
            if (!ring_ready_)
            {
                return;
            }

            io_uring_queue_exit(&ring_);
            std::memset(&ring_, 0, sizeof(ring_));
            ring_ready_ = false;
            accept_pending_ = false;
        }

        bool submit_accept(sockaddr_storage& client_storage, socklen_t& client_len)
        {
            std::lock_guard<std::mutex> lock(ring_mutex_);
            if (!ring_ready_ || accept_pending_ || listen_fd_ == -1)
            {
                return false;
            }

            auto* sqe = io_uring_get_sqe(&ring_);
            if (sqe == nullptr)
            {
                return false;
            }

            io_uring_prep_accept(
                sqe, listen_fd_, reinterpret_cast<sockaddr*>(&client_storage), &client_len, SOCK_NONBLOCK | SOCK_CLOEXEC);
            io_uring_sqe_set_data64(sqe, accept_user_data);

            if (io_uring_submit(&ring_) < 0)
            {
                return false;
            }

            accept_pending_ = true;
            return true;
        }

        auto poll_accept_completion() -> std::optional<int>
        {
            std::lock_guard<std::mutex> lock(ring_mutex_);
            if (!ring_ready_)
            {
                return -ECANCELED;
            }

            io_uring_cqe* cqe = nullptr;
            if (io_uring_peek_cqe(&ring_, &cqe) != 0 || cqe == nullptr)
            {
                return std::nullopt;
            }

            int result = cqe->res;
            auto user_data = io_uring_cqe_get_data64(cqe);
            io_uring_cqe_seen(&ring_, cqe);

            if (user_data == accept_user_data)
            {
                accept_pending_ = false;
                return result;
            }

            return std::nullopt;
        }

        void cancel_pending_accept()
        {
            std::lock_guard<std::mutex> lock(ring_mutex_);
            if (!ring_ready_ || !accept_pending_)
            {
                return;
            }

            auto* sqe = io_uring_get_sqe(&ring_);
            if (sqe == nullptr)
            {
                return;
            }

            io_uring_prep_cancel64(sqe, accept_user_data, 0);
            io_uring_sqe_set_data64(sqe, 0);
            (void)io_uring_submit(&ring_);
        }

        coro::net::socket_address endpoint_;
        coro::net::tcp::server::options opts_;
        std::shared_ptr<coro::scheduler> scheduler_;
        int listen_fd_ = -1;
        io_uring ring_{};
        std::mutex ring_mutex_;
        std::atomic<bool> stop_{false};
        bool ring_ready_ = false;
        bool accept_pending_ = false;
    };
} // namespace streaming::io_uring_tcp

#endif // __linux__
