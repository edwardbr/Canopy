// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#pragma once

#ifdef __linux__

#include <streaming/stream.h>

#include <coro/net/tcp/client.hpp>

#include <arpa/inet.h>
#include <coroutine>
#include <chrono>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <liburing.h>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <optional>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <thread>
#include <unordered_map>
#include <unistd.h>

namespace streaming
{
    class iouring_acceptor;
    class io_uring_stream_acceptor;

    namespace detail
    {
        inline auto translate_status(int error_code) -> coro::net::io_status
        {
            return coro::net::make_io_status_from_native(error_code);
        }
    }

    class iouring_stream : public stream
    {
    public:
        iouring_stream(coro::net::tcp::client&& client, std::shared_ptr<coro::scheduler> scheduler)
            : fd_(client.socket().native_handle())
            , scheduler_(std::move(scheduler))
            , state_(std::make_shared<ring_state>())
            , client_(std::move(client))
        {
            initialise_stream();
        }

        ~iouring_stream() override
        {
            set_closed();

            if (state_)
            {
                constexpr auto wait_limit = std::chrono::milliseconds(500);
                auto start = std::chrono::steady_clock::now();
                while (state_->pump_running.load(std::memory_order_acquire)
                       && std::chrono::steady_clock::now() - start < wait_limit)
                {
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                }
            }

            teardown_ring();
            close_owned_socket();
        }

        auto receive(rpc::mutable_byte_span buffer, std::chrono::milliseconds timeout = std::chrono::milliseconds{0})
            -> coro::task<std::pair<coro::net::io_status, rpc::mutable_byte_span>> override
        {
            if (closed_.load(std::memory_order_acquire))
            {
                co_return {coro::net::io_status{coro::net::io_status::kind::closed}, {}};
            }

            auto op = submit_recv(buffer, timeout);
            auto result = co_await operation_awaitable{op};

            if (result > 0)
            {
                co_return {coro::net::io_status{coro::net::io_status::kind::ok},
                    buffer.subspan(0, static_cast<size_t>(result))};
            }

            if (result == 0)
            {
                closed_.store(true, std::memory_order_release);
                co_return {coro::net::io_status{coro::net::io_status::kind::closed}, {}};
            }

            int native_error = -result;
            if (native_error == ECANCELED)
            {
                if (closed_.load(std::memory_order_acquire) || state_->stopping.load(std::memory_order_acquire))
                {
                    co_return {coro::net::io_status{coro::net::io_status::kind::closed}, {}};
                }
                co_return {coro::net::io_status{coro::net::io_status::kind::timeout}, {}};
            }

            if (native_error == ETIME)
            {
                co_return {coro::net::io_status{coro::net::io_status::kind::timeout}, {}};
            }

            if (native_error == EAGAIN || native_error == EWOULDBLOCK || native_error == EINTR)
            {
                co_return {coro::net::io_status{coro::net::io_status::kind::would_block_or_try_again}, {}};
            }

            closed_.store(true, std::memory_order_release);
            co_return {detail::translate_status(native_error), {}};
        }

        auto send(rpc::byte_span buffer) -> coro::task<coro::net::io_status> override
        {
            while (!buffer.empty())
            {
                if (closed_.load(std::memory_order_acquire))
                {
                    co_return coro::net::io_status{coro::net::io_status::kind::closed};
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
                    co_return coro::net::io_status{coro::net::io_status::kind::closed};
                }

                int native_error = -result;
                if (native_error == EAGAIN || native_error == EWOULDBLOCK || native_error == EINTR)
                {
                    co_await scheduler_->schedule();
                    continue;
                }

                if (native_error == ECANCELED && closed_.load(std::memory_order_acquire))
                {
                    co_return coro::net::io_status{coro::net::io_status::kind::closed};
                }

                closed_.store(true, std::memory_order_release);
                co_return detail::translate_status(native_error);
            }

            co_return coro::net::io_status{coro::net::io_status::kind::ok};
        }

        bool is_closed() const override { return closed_.load(std::memory_order_acquire); }

        void set_closed() override
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

        peer_info get_peer_info() const override
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
        struct accepted_socket_tag
        {
        };

        iouring_stream(accepted_socket_tag, int fd, std::shared_ptr<coro::scheduler> scheduler)
            : fd_(fd)
            , scheduler_(std::move(scheduler))
            , state_(std::make_shared<ring_state>())
        {
            initialise_stream();
        }

        static auto create_from_accepted_socket(int fd, std::shared_ptr<coro::scheduler> scheduler)
            -> std::shared_ptr<iouring_stream>
        {
            return std::shared_ptr<iouring_stream>(new iouring_stream(accepted_socket_tag{}, fd, std::move(scheduler)));
        }

        friend class iouring_acceptor;
        friend class io_uring_stream_acceptor;

        void initialise_stream()
        {
            if (fd_ < 0)
            {
                throw std::runtime_error("iouring_stream requires a valid socket");
            }

            int flag = 1;
            ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&flag), sizeof(flag));

            setup_ring();

            if (!scheduler_ || !scheduler_->spawn_detached(completion_pump(state_, scheduler_)))
            {
                teardown_ring();
                throw std::runtime_error("failed to start iouring_stream completion pump");
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

        struct pending_op
        {
            uint64_t id{0};
            int result{-ECANCELED};
            bool done{false};
            std::optional<__kernel_timespec> timeout_spec;
            std::coroutine_handle<> continuation{};
            std::mutex mutex;
        };

        struct ring_state
        {
            io_uring ring{};
            int event_fd{-1};
            std::atomic<bool> stopping{false};
            std::atomic<bool> pump_running{false};
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

            bool await_ready() const
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

            int await_resume() const
            {
                std::lock_guard<std::mutex> lock(op_->mutex);
                return op_->result;
            }

        private:
            std::shared_ptr<pending_op> op_;
        };

        static auto completion_pump(std::shared_ptr<ring_state> state, std::shared_ptr<coro::scheduler> scheduler)
            -> coro::task<void>
        {
            state->pump_running.store(true, std::memory_order_release);

            while (!state->stopping.load(std::memory_order_acquire))
            {
                drain_eventfd(state, scheduler);
                co_await scheduler->yield_for(std::chrono::milliseconds{1});
            }

            drain_eventfd(state, scheduler);
            fail_all_pending(state, scheduler, ECANCELED);
            state->pump_running.store(false, std::memory_order_release);
        }

        static void drain_eventfd(const std::shared_ptr<ring_state>& state, const std::shared_ptr<coro::scheduler>& scheduler)
        {
            if (state->event_fd == -1)
            {
                drain_cq(state, scheduler);
                return;
            }

            uint64_t counter = 0;
            while (::read(state->event_fd, &counter, sizeof(counter)) == sizeof(counter))
            {
            }

            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            {
                fail_all_pending(state, scheduler, errno);
                return;
            }

            drain_cq(state, scheduler);
        }

        static void drain_cq(const std::shared_ptr<ring_state>& state, const std::shared_ptr<coro::scheduler>& scheduler)
        {
            io_uring_cqe* cqe = nullptr;
            while (io_uring_peek_cqe(&state->ring, &cqe) == 0 && cqe != nullptr)
            {
                auto user_data = io_uring_cqe_get_data64(cqe);
                auto continuation = std::coroutine_handle<>{};
                std::shared_ptr<pending_op> op;

                {
                    std::lock_guard<std::mutex> state_lock(state->mutex);
                    auto it = state->in_flight.find(user_data);
                    if (it != state->in_flight.end())
                    {
                        op = std::move(it->second);
                        state->in_flight.erase(it);
                    }
                }

                if (op)
                {
                    std::lock_guard<std::mutex> op_lock(op->mutex);
                    op->result = cqe->res;
                    op->done = true;
                    continuation = op->continuation;
                }

                io_uring_cqe_seen(&state->ring, cqe);

                if (continuation)
                {
                    scheduler->resume(continuation);
                }

                cqe = nullptr;
            }
        }

        static void fail_all_pending(
            const std::shared_ptr<ring_state>& state, const std::shared_ptr<coro::scheduler>& scheduler, int error_code)
        {
            std::unordered_map<uint64_t, std::shared_ptr<pending_op>> ops;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                ops.swap(state->in_flight);
            }

            for (auto& [id, op] : ops)
            {
                (void)id;
                std::coroutine_handle<> continuation{};
                {
                    std::lock_guard<std::mutex> op_lock(op->mutex);
                    op->result = -error_code;
                    op->done = true;
                    continuation = op->continuation;
                }

                if (continuation)
                {
                    scheduler->resume(continuation);
                }
            }
        }

        auto submit_recv(rpc::mutable_byte_span buffer, std::chrono::milliseconds timeout) -> std::shared_ptr<pending_op>
        {
            auto op = std::make_shared<pending_op>();
            op->id = state_->next_id.fetch_add(1, std::memory_order_relaxed);

            std::lock_guard<std::mutex> lock(state_->mutex);

            auto* recv_sqe = io_uring_get_sqe(&state_->ring);
            if (recv_sqe == nullptr)
            {
                throw std::runtime_error("io_uring SQ is full during receive");
            }

            io_uring_prep_recv(recv_sqe, fd_, buffer.data(), static_cast<unsigned>(buffer.size()), 0);
            io_uring_sqe_set_data64(recv_sqe, op->id);

            if (timeout > std::chrono::milliseconds::zero())
            {
                auto* timeout_sqe = io_uring_get_sqe(&state_->ring);
                if (timeout_sqe == nullptr)
                {
                    throw std::runtime_error("io_uring SQ is full during receive timeout");
                }

                recv_sqe->flags |= IOSQE_IO_LINK;

                op->timeout_spec = __kernel_timespec{};
                op->timeout_spec->tv_sec = static_cast<__kernel_time64_t>(timeout.count() / 1000);
                op->timeout_spec->tv_nsec = static_cast<long>((timeout.count() % 1000) * 1000000LL);
                io_uring_prep_link_timeout(timeout_sqe, &*op->timeout_spec, 0);
                io_uring_sqe_set_data64(timeout_sqe, 0);
            }

            state_->in_flight.emplace(op->id, op);

            int submit_result = io_uring_submit(&state_->ring);
            if (submit_result < 0)
            {
                state_->in_flight.erase(op->id);
                throw std::runtime_error("io_uring_submit failed during receive");
            }

            return op;
        }

        auto submit_send(rpc::byte_span buffer) -> std::shared_ptr<pending_op>
        {
            auto op = std::make_shared<pending_op>();
            op->id = state_->next_id.fetch_add(1, std::memory_order_relaxed);

            std::lock_guard<std::mutex> lock(state_->mutex);

            auto* sqe = io_uring_get_sqe(&state_->ring);
            if (sqe == nullptr)
            {
                throw std::runtime_error("io_uring SQ is full during send");
            }

            io_uring_prep_send(sqe, fd_, buffer.data(), static_cast<unsigned>(buffer.size()), MSG_NOSIGNAL);
            io_uring_sqe_set_data64(sqe, op->id);
            state_->in_flight.emplace(op->id, op);

            int submit_result = io_uring_submit(&state_->ring);
            if (submit_result < 0)
            {
                state_->in_flight.erase(op->id);
                throw std::runtime_error("io_uring_submit failed during send");
            }

            return op;
        }

        void cancel_all()
        {
            if (!state_)
            {
                return;
            }

            std::lock_guard<std::mutex> lock(state_->mutex);
            for (const auto& [id, op] : state_->in_flight)
            {
                (void)op;
                auto* sqe = io_uring_get_sqe(&state_->ring);
                if (sqe == nullptr)
                {
                    break;
                }

                io_uring_prep_cancel64(sqe, id, 0);
                io_uring_sqe_set_data64(sqe, 0);
            }

            io_uring_submit(&state_->ring);
        }

        void setup_ring()
        {
            if (!state_)
            {
                return;
            }

            if (io_uring_queue_init(32, &state_->ring, 0) < 0)
            {
                throw std::runtime_error("io_uring_queue_init failed");
            }

            state_->event_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
            if (state_->event_fd == -1)
            {
                throw std::runtime_error("eventfd creation failed");
            }

            if (io_uring_register_eventfd(&state_->ring, state_->event_fd) < 0)
            {
                throw std::runtime_error("io_uring_register_eventfd failed");
            }
        }

        void teardown_ring()
        {
            if (!state_)
            {
                return;
            }

            bool was_running = state_->pump_running.exchange(false, std::memory_order_acq_rel);
            (void)was_running;

            if (state_->event_fd != -1)
            {
                ::close(state_->event_fd);
                state_->event_fd = -1;
            }

            io_uring_queue_exit(&state_->ring);
            state_.reset();
        }

        void signal_eventfd()
        {
            if (!state_ || state_->event_fd == -1)
            {
                return;
            }

            uint64_t value = 1;
            (void)::write(state_->event_fd, &value, sizeof(value));
        }

        int fd_{-1};
        std::shared_ptr<coro::scheduler> scheduler_;
        std::shared_ptr<ring_state> state_;
        std::optional<coro::net::tcp::client> client_;
        std::atomic<bool> closed_{false};
    };

} // namespace streaming

#endif // __linux__
