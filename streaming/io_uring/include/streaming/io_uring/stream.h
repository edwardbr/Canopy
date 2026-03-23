// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#pragma once

#ifdef __linux__

#  include <streaming/stream.h>

#  include <coro/net/tcp/client.hpp>

#  include <arpa/inet.h>
#  include <atomic>
#  include <cerrno>
#  include <chrono>
#  include <coroutine>
#  include <cstring>
#  include <liburing.h>
#  include <memory>
#  include <mutex>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <optional>
#  include <stdexcept>
#  include <sys/eventfd.h>
#  include <sys/socket.h>
#  include <thread>
#  include <unordered_map>
#  include <unistd.h>

namespace streaming::io_uring_tcp
{
    class acceptor;
}

namespace streaming::io_uring
{
    class acceptor;

    namespace detail
    {
        inline auto translate_status(int error_code) -> coro::net::io_status
        {
            return coro::net::make_io_status_from_native(error_code);
        }
    }

    class stream : public ::streaming::stream
    {
    public:
        static constexpr uint64_t timeout_user_data_flag = 1ULL << 63;

        stream(coro::net::tcp::client&& client, std::shared_ptr<coro::scheduler> scheduler)
            : fd_(client.socket().native_handle())
            , scheduler_(std::move(scheduler))
            , state_(std::make_shared<ring_state>())
            , client_(std::move(client))
        {
            initialise_stream();
        }

        ~stream() override = default;

        auto receive(rpc::mutable_byte_span buffer, std::chrono::milliseconds timeout = std::chrono::milliseconds{0})
            -> coro::task<std::pair<coro::net::io_status, rpc::mutable_byte_span>> override
        {
            if (closed_.load(std::memory_order_acquire))
            {
                co_return {coro::net::io_status{.type = coro::net::io_status::kind::closed}, {}};
            }

            const bool use_deadline = timeout.count() > 0;
            const auto deadline = use_deadline ? std::optional(std::chrono::steady_clock::now() + timeout) : std::nullopt;

            while (true)
            {
                auto remaining_timeout = std::chrono::milliseconds{0};
                if (deadline.has_value())
                {
                    const auto now = std::chrono::steady_clock::now();
                    if (now >= *deadline)
                    {
                        co_return {coro::net::io_status{.type = coro::net::io_status::kind::timeout}, {}};
                    }

                    remaining_timeout = std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - now);
                    if (remaining_timeout <= std::chrono::milliseconds::zero())
                    {
                        remaining_timeout = std::chrono::milliseconds{1};
                    }
                }

                auto op = submit_recv(buffer, remaining_timeout);
                auto result = co_await operation_awaitable{op};

                if (result > 0)
                {
                    co_return {coro::net::io_status{.type = coro::net::io_status::kind::ok},
                        buffer.subspan(0, static_cast<size_t>(result))};
                }

                if (result == 0)
                {
                    closed_.store(true, std::memory_order_release);
                    co_return {coro::net::io_status{.type = coro::net::io_status::kind::closed}, {}};
                }

                int native_error = -result;
                if (native_error == ECANCELED)
                {
                    if (closed_.load(std::memory_order_acquire) || state_->stopping.load(std::memory_order_acquire))
                    {
                        co_return {coro::net::io_status{.type = coro::net::io_status::kind::closed}, {}};
                    }
                    co_return {coro::net::io_status{.type = coro::net::io_status::kind::timeout}, {}};
                }

                if (native_error == ETIME)
                {
                    co_return {coro::net::io_status{.type = coro::net::io_status::kind::timeout}, {}};
                }

                if (native_error == EAGAIN || native_error == EWOULDBLOCK || native_error == EINTR)
                {
                    if (!deadline.has_value())
                    {
                        co_return {coro::net::io_status{.type = coro::net::io_status::kind::would_block_or_try_again}, {}};
                    }

                    co_await scheduler_->schedule();
                    continue;
                }

                closed_.store(true, std::memory_order_release);
                co_return {detail::translate_status(native_error), {}};
            }
        }

        auto send(rpc::byte_span buffer) -> coro::task<coro::net::io_status> override
        {
            while (!buffer.empty())
            {
                if (closed_.load(std::memory_order_acquire))
                {
                    co_return coro::net::io_status{.type = coro::net::io_status::kind::closed};
                }

                auto op = submit_send(buffer);
                auto result = co_await operation_awaitable{op};

                if (result > 0)
                {
                    buffer = buffer.subspan(static_cast<size_t>(result));
                    continue;
                }

                if (result == 0)
                {
                    closed_.store(true, std::memory_order_release);
                    co_return coro::net::io_status{.type = coro::net::io_status::kind::closed};
                }

                int native_error = -result;
                if (native_error == EAGAIN || native_error == EWOULDBLOCK || native_error == EINTR)
                {
                    co_await scheduler_->schedule();
                    continue;
                }

                if (native_error == ECANCELED && closed_.load(std::memory_order_acquire))
                {
                    co_return coro::net::io_status{.type = coro::net::io_status::kind::closed};
                }

                closed_.store(true, std::memory_order_release);
                co_return detail::translate_status(native_error);
            }

            co_return coro::net::io_status{.type = coro::net::io_status::kind::ok};
        }

        [[nodiscard]] auto is_closed() const -> bool override { return closed_.load(std::memory_order_acquire); }

        auto set_closed() -> coro::task<void> override
        {
            request_close();

            if (!state_)
            {
                close_owned_socket();
                co_return;
            }

            if (state_->pump_running.load(std::memory_order_acquire))
            {
                co_await state_->pump_stopped;
            }

            teardown_ring();
            close_owned_socket();
            co_return;
        }

        [[nodiscard]] auto get_peer_info() const -> peer_info override
        {
            peer_info info{};
            sockaddr_storage ss{};
            socklen_t len = sizeof(ss);
            if (fd_ == -1 || ::getpeername(fd_, reinterpret_cast<sockaddr*>(&ss), &len) != 0)
            {
                return info;
            }

            if (ss.ss_family == AF_INET)
            {
                const auto* sin = reinterpret_cast<const sockaddr_in*>(&ss);
                uint32_t addr = ntohl(sin->sin_addr.s_addr);
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

    private:
        void request_close()
        {
            bool was_closed = closed_.exchange(true, std::memory_order_acq_rel);

            if (state_)
            {
                state_->stopping.store(true, std::memory_order_release);
                cancel_all();
                signal_eventfd();
            }

            if (!was_closed && fd_ != -1)
            {
                if (client_)
                {
                    client_->socket().shutdown();
                }
                else
                {
                    ::shutdown(fd_, SHUT_RDWR);
                }
            }
        }
        struct accepted_socket_tag
        {
        };

        stream(accepted_socket_tag, int fd, std::shared_ptr<coro::scheduler> scheduler)
            : fd_(fd)
            , scheduler_(std::move(scheduler))
            , state_(std::make_shared<ring_state>())
        {
            initialise_stream();
        }

        static auto create_from_accepted_socket(int fd, std::shared_ptr<coro::scheduler> scheduler)
            -> std::shared_ptr<stream>
        {
            return std::shared_ptr<stream>(new stream(accepted_socket_tag{}, fd, std::move(scheduler)));
        }

        friend class acceptor;
        friend class ::streaming::io_uring_tcp::acceptor;

        struct pending_op
        {
            uint64_t id{0};
            int result{-ECANCELED};
            bool done{false};
            bool timeout_expired{false};
            std::optional<__kernel_timespec> timeout_spec;
            std::coroutine_handle<> continuation{};
            std::mutex mutex;
        };

        struct ring_state
        {
            struct io_uring ring{};
            int event_fd{-1};
            std::atomic<bool> stopping{false};
            std::atomic<bool> pump_running{false};
            coro::event pump_stopped{};
            std::atomic<bool> ring_torn_down{false};
            std::atomic<bool> cancel_requested{false};
            std::atomic<uint64_t> next_id{1};
            std::mutex mutex;
            std::unordered_map<uint64_t, std::shared_ptr<pending_op>> in_flight;
        };

        class operation_awaitable
        {
        public:
            explicit operation_awaitable(std::shared_ptr<pending_op> op)
                : op_(std::move(op))
            {
            }

            [[nodiscard]] bool await_ready() const
            {
                std::lock_guard<std::mutex> lock(op_->mutex);
                return op_->done;
            }

            bool await_suspend(std::coroutine_handle<> continuation)
            {
                std::lock_guard<std::mutex> lock(op_->mutex);
                if (op_->done)
                {
                    return false;
                }

                op_->continuation = continuation;
                return true;
            }

            [[nodiscard]] auto await_resume() const -> int
            {
                std::lock_guard<std::mutex> lock(op_->mutex);
                return op_->result;
            }

        private:
            std::shared_ptr<pending_op> op_;
        };

        void initialise_stream()
        {
            if (fd_ < 0)
            {
                throw std::runtime_error("io_uring::stream requires a valid socket");
            }

            int flag = 1;
            ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&flag), sizeof(flag));

            setup_ring();

            if (!scheduler_ || !scheduler_->spawn_detached(completion_pump(state_, scheduler_)))
            {
                teardown_ring();
                throw std::runtime_error("failed to start io_uring::stream completion pump");
            }
        }

        void close_owned_socket()
        {
            if (client_)
            {
                client_->socket().close();
                client_.reset();
                fd_ = -1;
                return;
            }

            if (fd_ != -1)
            {
                ::close(fd_);
                fd_ = -1;
            }
        }

        void setup_ring()
        {
            if (io_uring_queue_init(256, &state_->ring, 0) < 0)
            {
                throw std::runtime_error("io_uring_queue_init failed");
            }

            state_->event_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
            if (state_->event_fd < 0)
            {
                io_uring_queue_exit(&state_->ring);
                throw std::runtime_error("eventfd creation failed");
            }

            if (io_uring_register_eventfd(&state_->ring, state_->event_fd) < 0)
            {
                ::close(state_->event_fd);
                state_->event_fd = -1;
                io_uring_queue_exit(&state_->ring);
                throw std::runtime_error("io_uring_register_eventfd failed");
            }
        }

        void teardown_ring()
        {
            if (!state_ || state_->ring_torn_down.exchange(true, std::memory_order_acq_rel))
            {
                return;
            }

            if (state_->event_fd != -1)
            {
                ::close(state_->event_fd);
                state_->event_fd = -1;
            }
            io_uring_queue_exit(&state_->ring);
        }

        auto submit_recv(rpc::mutable_byte_span buffer, std::chrono::milliseconds timeout) -> std::shared_ptr<pending_op>
        {
            auto op = std::make_shared<pending_op>();
            op->id = state_->next_id.fetch_add(1, std::memory_order_relaxed);

            {
                std::lock_guard<std::mutex> lock(state_->mutex);
                state_->in_flight[op->id] = op;
            }

            const unsigned required_sqes = timeout.count() > 0 ? 2U : 1U;
            if (io_uring_sq_space_left(&state_->ring) < required_sqes)
            {
                std::lock_guard<std::mutex> state_lock(state_->mutex);
                state_->in_flight.erase(op->id);
                std::lock_guard<std::mutex> lock(op->mutex);
                op->done = true;
                op->result = -EAGAIN;
                return op;
            }

            io_uring_sqe* sqe = io_uring_get_sqe(&state_->ring);
            if (sqe == nullptr)
            {
                std::lock_guard<std::mutex> state_lock(state_->mutex);
                state_->in_flight.erase(op->id);
                std::lock_guard<std::mutex> lock(op->mutex);
                op->done = true;
                op->result = -EAGAIN;
                return op;
            }

            io_uring_prep_recv(sqe, fd_, buffer.data(), buffer.size(), 0);
            io_uring_sqe_set_data64(sqe, op->id);

            if (timeout.count() > 0)
            {
                io_uring_sqe* timeout_sqe = io_uring_get_sqe(&state_->ring);
                if (timeout_sqe == nullptr)
                {
                    std::lock_guard<std::mutex> state_lock(state_->mutex);
                    state_->in_flight.erase(op->id);
                    std::lock_guard<std::mutex> lock(op->mutex);
                    op->done = true;
                    op->result = -EAGAIN;
                    return op;
                }

                sqe->flags |= IOSQE_IO_LINK;
                op->timeout_spec = __kernel_timespec{
                    .tv_sec = static_cast<__kernel_time64_t>(timeout.count() / 1000),
                    .tv_nsec = static_cast<long>((timeout.count() % 1000) * 1000000),
                };
                io_uring_prep_link_timeout(timeout_sqe, &*op->timeout_spec, 0);
                io_uring_sqe_set_data64(timeout_sqe, op->id | timeout_user_data_flag);
            }

            if (io_uring_submit(&state_->ring) < 0)
            {
                std::lock_guard<std::mutex> state_lock(state_->mutex);
                state_->in_flight.erase(op->id);
                std::lock_guard<std::mutex> lock(op->mutex);
                op->done = true;
                op->result = -EIO;
                return op;
            }

            return op;
        }

        auto submit_send(rpc::byte_span buffer) -> std::shared_ptr<pending_op>
        {
            auto op = std::make_shared<pending_op>();
            op->id = state_->next_id.fetch_add(1, std::memory_order_relaxed);

            {
                std::lock_guard<std::mutex> lock(state_->mutex);
                state_->in_flight[op->id] = op;
            }

            io_uring_sqe* sqe = io_uring_get_sqe(&state_->ring);
            if (sqe == nullptr)
            {
                std::lock_guard<std::mutex> state_lock(state_->mutex);
                state_->in_flight.erase(op->id);
                std::lock_guard<std::mutex> lock(op->mutex);
                op->done = true;
                op->result = -EAGAIN;
                return op;
            }

            io_uring_prep_send(sqe, fd_, buffer.data(), buffer.size(), MSG_NOSIGNAL);
            io_uring_sqe_set_data64(sqe, op->id);

            if (io_uring_submit(&state_->ring) < 0)
            {
                std::lock_guard<std::mutex> state_lock(state_->mutex);
                state_->in_flight.erase(op->id);
                std::lock_guard<std::mutex> lock(op->mutex);
                op->done = true;
                op->result = -EIO;
                return op;
            }

            return op;
        }

        void cancel_all()
        {
            bool expected = false;
            if (!state_ || !state_->cancel_requested.compare_exchange_strong(expected, true))
            {
                return;
            }

            std::lock_guard<std::mutex> lock(state_->mutex);
            for (auto& [id, op] : state_->in_flight)
            {
                (void)op;
                io_uring_sqe* sqe = io_uring_get_sqe(&state_->ring);
                if (sqe == nullptr)
                {
                    break;
                }
                io_uring_prep_cancel64(sqe, id, 0);
                io_uring_sqe_set_data64(sqe, 0);
            }
            (void)io_uring_submit(&state_->ring);
        }

        void signal_eventfd()
        {
            if (state_->event_fd != -1)
            {
                uint64_t one = 1;
                (void)::write(state_->event_fd, &one, sizeof(one));
            }
        }

        static auto completion_pump(std::shared_ptr<ring_state> state, std::shared_ptr<coro::scheduler> scheduler)
            -> coro::task<void>
        {
            state->pump_stopped.reset();
            state->pump_running.store(true, std::memory_order_release);
            __kernel_timespec wait_timeout{
                .tv_sec = 0,
                .tv_nsec = 100'000,
            };

            auto fail_all_pending = [&](int native_error)
            {
                std::vector<std::coroutine_handle<>> continuations;
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    for (auto& [id, op] : state->in_flight)
                    {
                        (void)id;
                        std::lock_guard<std::mutex> op_lock(op->mutex);
                        op->done = true;
                        op->result = -native_error;
                        if (op->continuation)
                        {
                            continuations.push_back(op->continuation);
                            op->continuation = {};
                        }
                    }
                    state->in_flight.clear();
                }

                for (auto continuation : continuations)
                {
                    scheduler->resume(continuation);
                }
            };

            while (true)
            {
                if (state->stopping.load(std::memory_order_acquire))
                {
                    break;
                }

                io_uring_cqe* cqe = nullptr;
                int rc = io_uring_wait_cqe_timeout(&state->ring, &cqe, &wait_timeout);
                if (rc == -ETIME || rc == -EAGAIN)
                {
                    if (state->stopping.load(std::memory_order_acquire))
                    {
                        break;
                    }
                    co_await scheduler->schedule();
                    continue;
                }
                if (rc < 0 || cqe == nullptr)
                {
                    fail_all_pending(ECANCELED);
                    break;
                }

                auto id = io_uring_cqe_get_data64(cqe);
                if (id != 0)
                {
                    const bool is_timeout_cqe = (id & timeout_user_data_flag) != 0;
                    if (is_timeout_cqe)
                    {
                        id &= ~timeout_user_data_flag;
                    }

                    std::shared_ptr<pending_op> op;
                    {
                        std::lock_guard<std::mutex> lock(state->mutex);
                        auto it = state->in_flight.find(id);
                        if (it != state->in_flight.end())
                        {
                            op = it->second;
                            if (!is_timeout_cqe)
                            {
                                state->in_flight.erase(it);
                            }
                        }
                    }

                    if (op)
                    {
                        if (is_timeout_cqe)
                        {
                            std::lock_guard<std::mutex> lock(op->mutex);
                            op->timeout_expired = true;
                        }
                        else
                        {
                            std::coroutine_handle<> continuation;
                            {
                                std::lock_guard<std::mutex> lock(op->mutex);
                                op->done = true;
                                op->result = (op->timeout_expired && cqe->res == -ECANCELED) ? -ETIME : cqe->res;
                                continuation = op->continuation;
                            }
                            if (continuation)
                            {
                                scheduler->resume(continuation);
                            }
                        }
                    }
                }

                io_uring_cqe_seen(&state->ring, cqe);
            }

            fail_all_pending(ECANCELED);
            state->pump_running.store(false, std::memory_order_release);
            state->pump_stopped.set();
            if (!state->ring_torn_down.exchange(true, std::memory_order_acq_rel))
            {
                if (state->event_fd != -1)
                {
                    ::close(state->event_fd);
                    state->event_fd = -1;
                }
                io_uring_queue_exit(&state->ring);
            }
            co_return;
        }

        int fd_{-1};
        std::shared_ptr<coro::scheduler> scheduler_;
        std::shared_ptr<ring_state> state_;
        std::optional<coro::net::tcp::client> client_;
        std::atomic<bool> closed_{false};
    };
} // namespace streaming::io_uring

#endif // __linux__
