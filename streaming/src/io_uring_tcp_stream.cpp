// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

// io_uring_tcp_stream.cpp - Linux io_uring-backed TCP stream implementation
#include <streaming/io_uring_tcp_stream.h>

#include <atomic>
#include <cerrno>
#include <coroutine>
#include <cstring>
#include <linux/io_uring.h>
#include <linux/time_types.h>
#include <mutex>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <rpc/rpc.h>
#include <stdexcept>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace streaming
{
    struct io_uring_tcp_stream::ring_state
    {
        struct pending_op
        {
            uint64_t id{0};
            ssize_t result{-ECANCELED};
            bool completed{false};
            bool cancel_requested{false};
            std::coroutine_handle<> waiter{};
            // Storage for linked-timeout SQE. The kernel reads this at io_uring_enter
            // time, so it only needs to be valid until io_uring_enter returns.
            __kernel_timespec timeout_ts{0, 0};
        };

        int ring_fd{-1};
        int event_fd{-1};
        void* sq_ring_ptr{nullptr};
        void* cq_ring_ptr{nullptr};
        io_uring_sqe* sqes{nullptr};
        io_uring_cqe* cqes{nullptr};
        size_t sq_ring_size{0};
        size_t cq_ring_size{0};
        uint32_t sq_entry_count{0};
        uint32_t* sq_head{nullptr};
        uint32_t* sq_tail{nullptr};
        uint32_t* sq_ring_mask{nullptr};
        uint32_t* sq_ring_entries{nullptr};
        uint32_t* sq_flags{nullptr};
        uint32_t* sq_array{nullptr};
        uint32_t* cq_head{nullptr};
        uint32_t* cq_tail{nullptr};
        uint32_t* cq_ring_mask{nullptr};
        std::mutex mutex_;
        std::unordered_map<uint64_t, std::shared_ptr<pending_op>> in_flight_;
        uint64_t next_id{1};
        std::atomic<bool> stopping{false};
        std::atomic<bool> cancel_all_requested{false};
    };

    namespace
    {
#if defined(CANOPY_IO_URING_SQPOLL)
        constexpr bool use_sqpoll = true;
#else
        constexpr bool use_sqpoll = false;
#endif

        constexpr uint32_t queue_depth = 256;
        constexpr uint64_t cancel_user_data_base = 1ULL << 63;
        // op IDs start from 1, so 0 is safe as a sentinel for linked-timeout CQEs,
        // which drain_cq skips when not found in in_flight_.
        constexpr uint64_t linked_timeout_user_data = 0;

        auto io_uring_setup(unsigned entries, io_uring_params* params) -> int
        {
            return static_cast<int>(::syscall(__NR_io_uring_setup, entries, params));
        }

        auto io_uring_enter(unsigned ring_fd, unsigned to_submit, unsigned min_complete, unsigned flags) -> int
        {
            return static_cast<int>(::syscall(__NR_io_uring_enter, ring_fd, to_submit, min_complete, flags, nullptr, 0));
        }

        auto io_uring_register(unsigned ring_fd, unsigned opcode, const void* arg, unsigned nr_args) -> int
        {
            return static_cast<int>(::syscall(__NR_io_uring_register, ring_fd, opcode, arg, nr_args));
        }

        auto translate_native_status(int native_error) -> coro::net::io_status
        {
            if (native_error == EAGAIN || native_error == EWOULDBLOCK)
            {
                return coro::net::io_status{coro::net::io_status::kind::would_block_or_try_again};
            }
            return coro::net::io_status{coro::net::io_status::kind::native, native_error};
        }

        class operation_awaitable
        {
        public:
            operation_awaitable(std::shared_ptr<io_uring_tcp_stream::ring_state> state,
                std::shared_ptr<io_uring_tcp_stream::ring_state::pending_op> op)
                : state_(std::move(state))
                , op_(std::move(op))
            {
            }

            bool await_ready() const noexcept { return false; }

            bool await_suspend(std::coroutine_handle<> handle) noexcept
            {
                std::lock_guard<std::mutex> lock(state_->mutex_);
                if (op_->completed)
                {
                    return false;
                }

                op_->waiter = handle;
                return true;
            }

            auto await_resume() const noexcept -> ssize_t { return op_->result; }

        private:
            std::shared_ptr<io_uring_tcp_stream::ring_state> state_;
            std::shared_ptr<io_uring_tcp_stream::ring_state::pending_op> op_;
        };

        auto next_user_data(const std::shared_ptr<io_uring_tcp_stream::ring_state>& state) -> uint64_t
        {
            std::lock_guard<std::mutex> lock(state->mutex_);
            return state->next_id++;
        }

        auto eventfd_drain(int event_fd) -> void
        {
            uint64_t counter = 0;
            while (::read(event_fd, &counter, sizeof(counter)) == sizeof(counter))
            {
            }
        }

        auto teardown_ring(const std::shared_ptr<io_uring_tcp_stream::ring_state>& state) -> void
        {
            if (state->event_fd >= 0)
            {
                ::close(state->event_fd);
                state->event_fd = -1;
            }

            if (state->sqes)
            {
                ::munmap(state->sqes, state->sq_entry_count * sizeof(io_uring_sqe));
                state->sqes = nullptr;
            }

            if (state->cq_ring_ptr && state->cq_ring_ptr != state->sq_ring_ptr)
            {
                ::munmap(state->cq_ring_ptr, state->cq_ring_size);
            }
            state->cq_ring_ptr = nullptr;

            if (state->sq_ring_ptr)
            {
                ::munmap(state->sq_ring_ptr, state->sq_ring_size);
            }
            state->sq_ring_ptr = nullptr;

            if (state->ring_fd >= 0)
            {
                ::close(state->ring_fd);
                state->ring_fd = -1;
            }
        }

        auto setup_ring(const std::shared_ptr<io_uring_tcp_stream::ring_state>& state) -> void
        {
            io_uring_params params{};
            if constexpr (use_sqpoll)
            {
                params.flags = IORING_SETUP_SQPOLL | IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_COOP_TASKRUN;
                params.sq_thread_idle = 2000;
            }

            state->ring_fd = io_uring_setup(queue_depth, &params);
            if (state->ring_fd < 0)
            {
                throw std::runtime_error("io_uring_setup failed");
            }

            state->sq_ring_size = params.sq_off.array + params.sq_entries * sizeof(uint32_t);
            state->cq_ring_size = params.cq_off.cqes + params.cq_entries * sizeof(io_uring_cqe);
            state->sq_entry_count = params.sq_entries;

            if (params.features & IORING_FEAT_SINGLE_MMAP)
            {
                state->sq_ring_size = std::max(state->sq_ring_size, state->cq_ring_size);
                state->cq_ring_size = state->sq_ring_size;
            }

            state->sq_ring_ptr = ::mmap(
                0, state->sq_ring_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, state->ring_fd, IORING_OFF_SQ_RING);
            if (state->sq_ring_ptr == MAP_FAILED)
            {
                state->sq_ring_ptr = nullptr;
                throw std::runtime_error("io_uring SQ mmap failed");
            }

            if (params.features & IORING_FEAT_SINGLE_MMAP)
            {
                state->cq_ring_ptr = state->sq_ring_ptr;
            }
            else
            {
                state->cq_ring_ptr = ::mmap(
                    0, state->cq_ring_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, state->ring_fd, IORING_OFF_CQ_RING);
                if (state->cq_ring_ptr == MAP_FAILED)
                {
                    state->cq_ring_ptr = nullptr;
                    throw std::runtime_error("io_uring CQ mmap failed");
                }
            }

            state->sqes = static_cast<io_uring_sqe*>(::mmap(0,
                params.sq_entries * sizeof(io_uring_sqe),
                PROT_READ | PROT_WRITE,
                MAP_SHARED | MAP_POPULATE,
                state->ring_fd,
                IORING_OFF_SQES));
            if (state->sqes == MAP_FAILED)
            {
                state->sqes = nullptr;
                throw std::runtime_error("io_uring SQE mmap failed");
            }

            state->sq_head = reinterpret_cast<uint32_t*>(static_cast<char*>(state->sq_ring_ptr) + params.sq_off.head);
            state->sq_tail = reinterpret_cast<uint32_t*>(static_cast<char*>(state->sq_ring_ptr) + params.sq_off.tail);
            state->sq_ring_mask
                = reinterpret_cast<uint32_t*>(static_cast<char*>(state->sq_ring_ptr) + params.sq_off.ring_mask);
            state->sq_ring_entries
                = reinterpret_cast<uint32_t*>(static_cast<char*>(state->sq_ring_ptr) + params.sq_off.ring_entries);
            state->sq_flags = reinterpret_cast<uint32_t*>(static_cast<char*>(state->sq_ring_ptr) + params.sq_off.flags);
            state->sq_array = reinterpret_cast<uint32_t*>(static_cast<char*>(state->sq_ring_ptr) + params.sq_off.array);

            state->cq_head = reinterpret_cast<uint32_t*>(static_cast<char*>(state->cq_ring_ptr) + params.cq_off.head);
            state->cq_tail = reinterpret_cast<uint32_t*>(static_cast<char*>(state->cq_ring_ptr) + params.cq_off.tail);
            state->cq_ring_mask
                = reinterpret_cast<uint32_t*>(static_cast<char*>(state->cq_ring_ptr) + params.cq_off.ring_mask);
            state->cqes = reinterpret_cast<io_uring_cqe*>(static_cast<char*>(state->cq_ring_ptr) + params.cq_off.cqes);

            state->event_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
            if (state->event_fd < 0)
            {
                throw std::runtime_error("eventfd failed");
            }

            if (io_uring_register(static_cast<unsigned>(state->ring_fd), IORING_REGISTER_EVENTFD, &state->event_fd, 1) < 0)
            {
                throw std::runtime_error("io_uring eventfd registration failed");
            }
        }

        template<class FillSqe>
        auto submit_sqe(const std::shared_ptr<io_uring_tcp_stream::ring_state>& state, FillSqe&& fill) -> bool
        {
            std::lock_guard<std::mutex> lock(state->mutex_);
            if (state->stopping.load(std::memory_order_acquire))
            {
                return false;
            }

            if ((*state->sq_tail - *state->sq_head) >= *state->sq_ring_entries)
            {
                return false;
            }

            uint32_t tail = *state->sq_tail;
            uint32_t index = tail & *state->sq_ring_mask;
            auto& sqe = state->sqes[index];
            std::memset(&sqe, 0, sizeof(sqe));
            fill(sqe);

            state->sq_array[index] = index;
            std::atomic_thread_fence(std::memory_order_release);
            *state->sq_tail = tail + 1;

            unsigned flags = 0;
            if (state->sq_flags && (*state->sq_flags & IORING_SQ_NEED_WAKEUP))
            {
                flags |= IORING_ENTER_SQ_WAKEUP;
            }

            return io_uring_enter(static_cast<unsigned>(state->ring_fd), 1, 0, flags) >= 0;
        }

        auto submit_recv(const std::shared_ptr<io_uring_tcp_stream::ring_state>& state,
            int fd,
            std::span<char> buffer,
            std::chrono::milliseconds timeout) -> std::shared_ptr<io_uring_tcp_stream::ring_state::pending_op>
        {
            auto op = std::make_shared<io_uring_tcp_stream::ring_state::pending_op>();
            bool submitted = false;

            std::lock_guard<std::mutex> lock(state->mutex_);
            if (state->stopping.load(std::memory_order_acquire))
            {
                op->result = -ECANCELED;
                op->completed = true;
                return op;
            }

            op->id = state->next_id++;
            state->in_flight_.emplace(op->id, op);

            if (timeout.count() > 0)
            {
                // Submit RECV linked to IORING_OP_LINK_TIMEOUT as a pair.
                // If the timeout fires first the RECV CQE returns -ECANCELED.
                // If the RECV completes first the LINK_TIMEOUT CQE returns -ECANCELED
                // with user_data = linked_timeout_user_data (0), which drain_cq skips.
                // The kernel reads timeout_ts at io_uring_enter time (inside this lock),
                // so it is safe to store it in pending_op and free the op later.
                op->timeout_ts.tv_sec = timeout.count() / 1000;
                op->timeout_ts.tv_nsec = (timeout.count() % 1000) * 1'000'000LL;

                if ((*state->sq_tail - *state->sq_head + 2) <= *state->sq_ring_entries)
                {
                    uint32_t tail = *state->sq_tail;

                    uint32_t i0 = tail & *state->sq_ring_mask;
                    auto& s0 = state->sqes[i0];
                    std::memset(&s0, 0, sizeof(s0));
                    s0.opcode = IORING_OP_RECV;
                    s0.flags = IOSQE_IO_LINK;
                    s0.fd = fd;
                    s0.addr = reinterpret_cast<uint64_t>(buffer.data());
                    s0.len = static_cast<uint32_t>(buffer.size());
                    s0.user_data = op->id;
                    state->sq_array[i0] = i0;

                    uint32_t i1 = (tail + 1) & *state->sq_ring_mask;
                    auto& s1 = state->sqes[i1];
                    std::memset(&s1, 0, sizeof(s1));
                    s1.opcode = IORING_OP_LINK_TIMEOUT;
                    s1.addr = reinterpret_cast<uint64_t>(&op->timeout_ts);
                    s1.len = 1;
                    s1.user_data = linked_timeout_user_data;
                    state->sq_array[i1] = i1;

                    std::atomic_thread_fence(std::memory_order_release);
                    *state->sq_tail = tail + 2;

                    unsigned flags = 0;
                    if (state->sq_flags && (*state->sq_flags & IORING_SQ_NEED_WAKEUP))
                        flags |= IORING_ENTER_SQ_WAKEUP;

                    submitted = io_uring_enter(static_cast<unsigned>(state->ring_fd), 2, 0, flags) >= 0;
                }
            }
            else
            {
                if ((*state->sq_tail - *state->sq_head) < *state->sq_ring_entries)
                {
                    uint32_t tail = *state->sq_tail;
                    uint32_t index = tail & *state->sq_ring_mask;
                    auto& sqe = state->sqes[index];
                    std::memset(&sqe, 0, sizeof(sqe));
                    sqe.opcode = IORING_OP_RECV;
                    sqe.fd = fd;
                    sqe.addr = reinterpret_cast<uint64_t>(buffer.data());
                    sqe.len = static_cast<uint32_t>(buffer.size());
                    sqe.user_data = op->id;
                    state->sq_array[index] = index;

                    std::atomic_thread_fence(std::memory_order_release);
                    *state->sq_tail = tail + 1;

                    unsigned flags = 0;
                    if (state->sq_flags && (*state->sq_flags & IORING_SQ_NEED_WAKEUP))
                        flags |= IORING_ENTER_SQ_WAKEUP;

                    submitted = io_uring_enter(static_cast<unsigned>(state->ring_fd), 1, 0, flags) >= 0;
                }
            }

            if (!submitted)
            {
                state->in_flight_.erase(op->id);
                op->result = -EAGAIN;
                op->completed = true;
            }

            return op;
        }

        auto submit_send(const std::shared_ptr<io_uring_tcp_stream::ring_state>& state, int fd, std::span<const char> buffer)
            -> std::shared_ptr<io_uring_tcp_stream::ring_state::pending_op>
        {
            auto op = std::make_shared<io_uring_tcp_stream::ring_state::pending_op>();
            op->id = next_user_data(state);

            {
                std::lock_guard<std::mutex> lock(state->mutex_);
                state->in_flight_.emplace(op->id, op);
            }

            if (!submit_sqe(state,
                    [fd, buffer, id = op->id](io_uring_sqe& sqe)
                    {
                        sqe.opcode = IORING_OP_SEND;
                        sqe.fd = fd;
                        sqe.addr = reinterpret_cast<uint64_t>(buffer.data());
                        sqe.len = static_cast<uint32_t>(buffer.size());
                        sqe.msg_flags = MSG_NOSIGNAL;
                        sqe.user_data = id;
                    }))
            {
                std::lock_guard<std::mutex> lock(state->mutex_);
                state->in_flight_.erase(op->id);
                op->result = -EAGAIN;
                op->completed = true;
            }

            return op;
        }

        auto cancel_op(const std::shared_ptr<io_uring_tcp_stream::ring_state>& state, uint64_t id) -> void
        {
            (void)submit_sqe(state,
                [id](io_uring_sqe& sqe)
                {
                    sqe.opcode = IORING_OP_ASYNC_CANCEL;
                    sqe.addr = id;
                    sqe.cancel_flags = IORING_ASYNC_CANCEL_USERDATA;
                    sqe.user_data = cancel_user_data_base | id;
                });
        }

        auto cancel_all_ops(const std::shared_ptr<io_uring_tcp_stream::ring_state>& state) -> void
        {
            bool expected = false;
            if (!state->cancel_all_requested.compare_exchange_strong(expected, true))
            {
                return;
            }

            std::vector<uint64_t> ids;
            {
                std::lock_guard<std::mutex> lock(state->mutex_);
                ids.reserve(state->in_flight_.size());
                for (auto& [id, op] : state->in_flight_)
                {
                    if (!op->cancel_requested)
                    {
                        op->cancel_requested = true;
                        ids.push_back(id);
                    }
                }
            }

            for (auto id : ids)
            {
                cancel_op(state, id);
            }
        }

        auto mark_all_failed(const std::shared_ptr<io_uring_tcp_stream::ring_state>& state,
            std::shared_ptr<coro::scheduler> scheduler,
            int native_error) -> void
        {
            std::vector<std::coroutine_handle<>> to_resume;
            {
                std::lock_guard<std::mutex> lock(state->mutex_);
                for (auto& [id, op] : state->in_flight_)
                {
                    (void)id;
                    op->result = -native_error;
                    op->completed = true;
                    if (op->waiter)
                    {
                        to_resume.push_back(op->waiter);
                        op->waiter = {};
                    }
                }
                state->in_flight_.clear();
            }

            for (auto handle : to_resume)
            {
                scheduler->resume(handle);
            }
        }

        auto drain_cq(const std::shared_ptr<io_uring_tcp_stream::ring_state>& state,
            std::shared_ptr<coro::scheduler> scheduler) -> void
        {
            std::vector<std::coroutine_handle<>> to_resume;

            {
                std::lock_guard<std::mutex> lock(state->mutex_);
                uint32_t head = *state->cq_head;
                const uint32_t tail = *state->cq_tail;

                while (head != tail)
                {
                    auto& cqe = state->cqes[head & *state->cq_ring_mask];
                    auto it = state->in_flight_.find(cqe.user_data);
                    if (it != state->in_flight_.end())
                    {
                        auto& op = it->second;
                        op->result = cqe.res;
                        op->completed = true;
                        if (op->waiter)
                        {
                            to_resume.push_back(op->waiter);
                            op->waiter = {};
                        }
                        state->in_flight_.erase(it);
                    }
                    ++head;
                }

                std::atomic_thread_fence(std::memory_order_release);
                *state->cq_head = head;
            }

            for (auto handle : to_resume)
            {
                scheduler->resume(handle);
            }
        }

        auto in_flight_count(const std::shared_ptr<io_uring_tcp_stream::ring_state>& state) -> size_t
        {
            std::lock_guard<std::mutex> lock(state->mutex_);
            return state->in_flight_.size();
        }
    } // namespace

    io_uring_tcp_stream::io_uring_tcp_stream(coro::net::tcp::client&& client, std::shared_ptr<coro::scheduler> scheduler)
        : client_(std::move(client))
        , scheduler_(std::move(scheduler))
        , state_(std::make_shared<ring_state>())
    {
        // Disable Nagle's algorithm so small RPC packets are sent immediately.
        // Without this, Nagle + delayed-ACK interaction causes ~40-80ms stalls per
        // round-trip for payloads smaller than one TCP segment.
        int flag = 1;
        ::setsockopt(
            client_.socket().native_handle(), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&flag), sizeof(flag));

        try
        {
            setup_ring(state_);
        }
        catch (...)
        {
            teardown_ring(state_);
            throw;
        }

        if (!scheduler_->spawn_detached(completion_pump(state_, scheduler_)))
        {
            teardown_ring(state_);
            throw std::runtime_error("failed to start io_uring completion pump");
        }
    }

    io_uring_tcp_stream::~io_uring_tcp_stream()
    {
        set_closed();
    }

    auto io_uring_tcp_stream::completion_pump(
        std::shared_ptr<ring_state> state, std::shared_ptr<coro::scheduler> scheduler) -> coro::task<void>
    {
        while (true)
        {
            if (state->stopping.load(std::memory_order_acquire))
            {
                cancel_all_ops(state);
                drain_cq(state, scheduler);
                if (in_flight_count(state) == 0)
                {
                    break;
                }
            }

            auto poll_status
                = co_await scheduler->poll(state->event_fd, coro::poll_op::read, std::chrono::milliseconds{50});
            if (poll_status == coro::poll_status::read)
            {
                eventfd_drain(state->event_fd);
                drain_cq(state, scheduler);
                continue;
            }

            if (poll_status != coro::poll_status::timeout)
            {
                mark_all_failed(state, scheduler, ECANCELED);
                break;
            }
        }

        teardown_ring(state);
    }

    auto io_uring_tcp_stream::receive(std::span<char> buffer, std::chrono::milliseconds timeout)
        -> coro::task<std::pair<coro::net::io_status, std::span<char>>>
    {
        if (closed_)
        {
            co_return std::pair{coro::net::io_status{coro::net::io_status::kind::closed}, std::span<char>{}};
        }

        while (true)
        {
            // Submit IORING_OP_RECV directly — io_uring waits for data asynchronously,
            // so no prior epoll poll() on the socket fd is needed or wanted. When a
            // timeout is provided we link an IORING_OP_LINK_TIMEOUT so the kernel
            // cancels the RECV and returns -ECANCELED if no data arrives in time.
            auto op = submit_recv(state_, client_.socket().native_handle(), buffer, timeout);
            auto result = co_await operation_awaitable(state_, op);

            if (result > 0)
            {
                co_return std::pair{coro::net::io_status{coro::net::io_status::kind::ok},
                    buffer.subspan(0, static_cast<size_t>(result))};
            }
            if (result == 0)
            {
                closed_ = true;
                co_return std::pair{coro::net::io_status{coro::net::io_status::kind::closed}, std::span<char>{}};
            }

            int native_error = static_cast<int>(-result);
            if (native_error == EAGAIN || native_error == EWOULDBLOCK || native_error == EINTR)
            {
                continue;
            }
            if (native_error == ECANCELED || native_error == ETIME)
            {
                // -ECANCELED: either the linked timeout fired, or set_closed() called
                //             cancel_all_ops(). Distinguish by checking stopping flag.
                // -ETIME:     linked timeout CQE (shouldn't reach here, but defensive).
                if (closed_ || state_->stopping.load(std::memory_order_acquire))
                {
                    co_return std::pair{coro::net::io_status{coro::net::io_status::kind::closed}, std::span<char>{}};
                }
                co_return std::pair{coro::net::io_status{coro::net::io_status::kind::timeout}, std::span<char>{}};
            }

            closed_ = true;
            co_return std::pair{translate_native_status(native_error), std::span<char>{}};
        }
    }

    auto io_uring_tcp_stream::send(std::span<const char> buffer) -> coro::task<coro::net::io_status>
    {
        if (closed_)
        {
            co_return coro::net::io_status{coro::net::io_status::kind::closed};
        }

        while (!buffer.empty())
        {
            auto op = submit_send(state_, client_.socket().native_handle(), buffer);
            auto result = co_await operation_awaitable(state_, op);
            if (result > 0)
            {
                buffer = buffer.subspan(static_cast<size_t>(result));
                continue;
            }
            if (result == 0)
            {
                closed_ = true;
                co_return coro::net::io_status{coro::net::io_status::kind::closed};
            }

            int native_error = static_cast<int>(-result);
            if (native_error == EAGAIN || native_error == EWOULDBLOCK || native_error == EINTR)
            {
                // Yield to the scheduler and retry rather than epoll-polling the socket
                // fd for write-readiness. Polling the fd while receive() may have it
                // registered for reads causes an EPOLL_CTL_ADD EEXIST conflict.
                co_await scheduler_->schedule();
                continue;
            }

            closed_ = true;
            co_return translate_native_status(native_error);
        }

        co_return coro::net::io_status{coro::net::io_status::kind::ok};
    }

    bool io_uring_tcp_stream::is_closed() const
    {
        return closed_;
    }

    void io_uring_tcp_stream::set_closed()
    {
        closed_ = true;
        if (state_)
        {
            state_->stopping.store(true, std::memory_order_release);
            cancel_all_ops(state_);
        }

        if (!socket_closed_)
        {
            socket_closed_ = true;
            client_.socket().shutdown();
            client_.socket().close();
        }
    }

    auto io_uring_tcp_stream::client() -> coro::net::tcp::client&
    {
        return client_;
    }

    peer_info io_uring_tcp_stream::get_peer_info() const
    {
        peer_info info{};
        sockaddr_storage ss{};
        socklen_t len = sizeof(ss);
        if (::getpeername(client_.socket().native_handle(), reinterpret_cast<sockaddr*>(&ss), &len) != 0)
        {
            return info;
        }

        if (ss.ss_family == AF_INET)
        {
            const auto* sin = reinterpret_cast<const sockaddr_in*>(&ss);
            const auto addr = ntohl(sin->sin_addr.s_addr);
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

} // namespace streaming
