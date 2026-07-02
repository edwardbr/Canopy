/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <atomic>
#include <array>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <io_uring/detail/operation_engine.h>
#include <io_uring/io_uring_handle.h>
#include <io_uring/types.h>
#include <rpc/rpc.h>

namespace rpc::io_uring
{
    class direct_descriptor;

    class controller : public std::enable_shared_from_this<controller>
    {
    public:
        struct options
        {
            // Selects who is responsible for draining the io_uring completion
            // queue while operations are outstanding:
            //
            //     cooperative_poll: each waiting coroutine pumps the CQ itself
            //     proactor:         a scheduler task pumps the CQ and resumes
            //                       the matching waiting coroutine
            //
            // Sensible default: leave this as cooperative_poll unless profiling
            // shows many concurrent direct io_uring operations spending too
            // much time in repeated CQ polling. Proactor is a throughput tuning
            // option for scheduler-backed workloads, not a requirement for
            // ordinary users of the controller.
            wait_strategy completion_wait_strategy{wait_strategy::cooperative_poll};
            // Linux MSG_* bits applied to direct TCP send/receive SQEs. Keep
            // these zero unless a caller deliberately wants socket-level
            // experiments without changing the stream or transport layers.
            uint32_t send_message_flags{0};
            uint32_t receive_message_flags{0};
            // Optimization: submit the caller's per-transfer
            // byte_span/mutable_byte_span address directly in SEND/RECV SQEs.
            // The fallback path stages bytes through the controller's staging
            // buffer pool.
            bool use_caller_buffers_for_transfers{false};
        };

        explicit controller(
            std::shared_ptr<io_uring_handle> handle = {},
            rpc::coro::scheduler* scheduler = nullptr);
        controller(
            std::shared_ptr<io_uring_handle> handle,
            rpc::coro::scheduler* scheduler,
            options controller_options);
        virtual ~controller() = default;

        void set_wait_strategy(wait_strategy strategy) noexcept;
        [[nodiscard]] wait_strategy get_wait_strategy() const noexcept;
        [[nodiscard]] options get_options() const noexcept;

        // Only reset measurements when the caller knows no operations from the
        // previous sample are still in flight.
        void reset_measurements() noexcept;
        [[nodiscard]] controller_measurements measurements() const noexcept;

        CORO_TASK(int) shutdown();
        void request_shutdown() noexcept;
        void request_shutdown(int error_code) noexcept;
        [[nodiscard]] bool is_shutdown_requested() const noexcept;

        CORO_TASK(int) wake_iouring();
        CORO_TASK(int)
        notify_submitted(
            data ring_data,
            uint32_t sqe_count);
        CORO_TASK(int) get_iouring_data(data& ring_data);
        CORO_TASK(int) refresh_iouring_data();
        void clear_iouring_data_cache() noexcept;
        [[nodiscard]] const data* cached_iouring_data() const noexcept;

        CORO_TASK(int) no_op();

        CORO_TASK(descriptor_result)
        open_file(
            std::string path,
            uint32_t open_flags,
            uint32_t mode);
        CORO_TASK(transfer_result)
        read_at(
            uint32_t descriptor,
            rpc::mutable_byte_span buffer,
            uint64_t offset);
        CORO_TASK(transfer_result)
        write_at(
            uint32_t descriptor,
            rpc::byte_span buffer,
            uint64_t offset);

        CORO_TASK(descriptor_result) create_tcp_ipv4_socket();
        CORO_TASK(descriptor_result) create_tcp_ipv6_socket();
        CORO_TASK(operation_result) set_socket_reuse_addr(uint32_t descriptor);
        CORO_TASK(operation_result)
        bind_tcp_ipv4_loopback(
            uint32_t descriptor,
            uint16_t port);
        CORO_TASK(operation_result)
        bind_tcp_ipv4(
            uint32_t descriptor,
            std::array<
                uint8_t,
                4> address,
            uint16_t port);
        CORO_TASK(operation_result)
        bind_tcp_ipv6(
            uint32_t descriptor,
            std::array<
                uint8_t,
                16> address,
            uint16_t port);
        CORO_TASK(operation_result)
        listen(
            uint32_t descriptor,
            uint32_t backlog);
        CORO_TASK(descriptor_result) accept(uint32_t listen_descriptor);
        CORO_TASK(descriptor_result)
        connect_tcp_ipv4_loopback(
            uint16_t port,
            std::chrono::milliseconds timeout);
        CORO_TASK(descriptor_result)
        connect_tcp_ipv4(
            std::array<
                uint8_t,
                4> address,
            uint16_t port,
            std::chrono::milliseconds timeout);
        CORO_TASK(descriptor_result)
        connect_tcp_ipv6(
            std::array<
                uint8_t,
                16> address,
            uint16_t port,
            std::chrono::milliseconds timeout);
        CORO_TASK(transfer_result)
        send(
            uint32_t descriptor,
            rpc::byte_span buffer);
        CORO_TASK(transfer_result)
        receive(
            uint32_t descriptor,
            rpc::mutable_byte_span buffer);
        CORO_TASK(transfer_result)
        receive_nonblocking(
            uint32_t descriptor,
            rpc::mutable_byte_span buffer);
        CORO_TASK(transfer_result)
        receive(
            uint32_t descriptor,
            rpc::mutable_byte_span buffer,
            std::chrono::milliseconds timeout);
        CORO_TASK(operation_result) cancel_direct(uint32_t descriptor);
        CORO_TASK(operation_result) close_direct(uint32_t descriptor);

    private:
        friend class direct_descriptor;

        // Direct io_uring can either point a send/receive SQE straight at the
        // caller's byte span:
        //
        //     caller span ------------------------------> SQE.addr
        //
        // Or borrow a slot from the staging pool described by ring_data.buffers:
        //
        //     caller memory              staging slot               kernel
        //     +---------------------+    +----------------------+    +------+
        //     | RPC buffers, stack  | -> | ring_data.buffers[N] | -> | SQE  |
        //     +---------------------+    +----------------------+    +------+
        //
        // SEND copies caller bytes into the staging slot before submission.
        // RECV submits the staging slot address, then copies completed bytes
        // back to the caller. Small kernel inputs such as sockaddr, timespec
        // and setsockopt values use the same pool for the same reason.
        //
        // staging_buffer is the RAII owner for one borrowed slot. Holding a
        // shared_ptr<staging_buffer> keeps the slot reserved until the SQE has
        // completed and any copy-back has happened.
        class staging_buffer
        {
        public:
            ~staging_buffer();

            staging_buffer(const staging_buffer&) = delete;
            staging_buffer& operator=(const staging_buffer&) = delete;
            staging_buffer(staging_buffer&&) = delete;
            staging_buffer& operator=(staging_buffer&&) = delete;

            [[nodiscard]] uint8_t* data() const noexcept;
            [[nodiscard]] size_t size() const noexcept;
            [[nodiscard]] uint64_t address() const noexcept;

        private:
            friend class controller;

            staging_buffer(
                controller* controller,
                uint32_t slot,
                uint64_t generation,
                uint8_t* data,
                size_t size) noexcept;

            controller* controller_{nullptr};
            uint32_t slot_{0};
            uint64_t generation_{0};
            uint8_t* data_{nullptr};
            size_t size_{0};
        };

        struct staging_buffer_allocation_result
        {
            int error_code{rpc::error::OK()};
            std::shared_ptr<staging_buffer> buffer;
        };

        struct staging_buffer_pair_allocation_result
        {
            int error_code{rpc::error::OK()};
            std::shared_ptr<staging_buffer> first_buffer;
            std::shared_ptr<staging_buffer> second_buffer;
        };

        struct staging_buffer_reservation
        {
            // A reservation is the lock-protected, allocation-free form of a
            // staging slot. We first reserve raw slots while holding
            // staging_buffer_mutex_, then construct shared_ptr guards after
            // leaving the spin-lock critical section.
            uint32_t slot{0};
            uint64_t generation{0};
            uint8_t* data{nullptr};
            size_t size{0};
        };

        struct staging_buffer_waiter
        {
            // Waiters queue for one or two staging slots. Two-slot reservations
            // are intentionally atomic: linked operations such as
            // RECV+LINK_TIMEOUT must not hold a data buffer while separately
            // waiting for a timeout buffer in a tiny pool.
            //
            //     available slot(s)
            //             |
            //             v
            //     next grantable waiter reserves the required slot(s)
            //             |
            //             v
            //     coroutine resumes and wraps reservations in staging_buffer
            uint32_t required_buffer_count{0};
            size_t first_requested_size{0};
            size_t second_requested_size{0};
            bool queued{false};
            bool granted{false};
            int error_code{rpc::error::OK()};
            std::coroutine_handle<> waiter;
            staging_buffer_reservation first_reservation;
            staging_buffer_reservation second_reservation;
        };

        struct submission_waiter
        {
            // Admission token for the SQ. This is separate from the operation
            // table lock: it keeps coroutines from repeatedly racing each other
            // for a full ring, and preserves FIFO ordering when the ring is
            // deliberately small in stress tests.
            uint32_t required_sqe_count{0};
            bool queued{false};
            int error_code{rpc::error::OK()};
        };

        struct atomic_measurements
        {
            void reset() noexcept;
            [[nodiscard]] controller_measurements snapshot() const noexcept;

            std::atomic<uint64_t> no_op_calls{0};
            std::atomic<uint64_t> no_op_successes{0};
            std::atomic<uint64_t> no_op_failures{0};
            std::atomic<uint64_t> submit_attempts{0};
            std::atomic<uint64_t> submit_backpressure{0};
            std::atomic<uint64_t> completion_pump_calls{0};
            std::atomic<uint64_t> completion_entries{0};
            std::atomic<uint64_t> scheduler_yields{0};
            std::atomic<uint64_t> local_relax_spins{0};
            std::atomic<uint64_t> host_wake_calls{0};
            std::atomic<uint64_t> proactor_pump_starts{0};
            std::atomic<uint64_t> proactor_pump_iterations{0};
            std::atomic<uint64_t> proactor_waiter_suspends{0};
            std::atomic<uint64_t> proactor_resumes{0};
            std::atomic<uint64_t> proactor_start_failures{0};
            std::atomic<uint64_t> total_no_op_ticks{0};
            std::atomic<uint64_t> max_no_op_ticks{0};
        };

        using fill_sqe_callback = void (*)(
            detail::sqe_64& sqe,
            void* context);
        using fill_linked_sqe_callback = void (*)(
            detail::sqe_64& primary_sqe,
            detail::sqe_64& linked_sqe,
            void* context);

        // The controller owns the lifetime around SQE.user_data:
        //
        //     caller coroutine
        //             |
        //             v
        //     direct_ring_operation --user_data--> SQE.user_data
        //             ^                              |
        //             |                              v
        //     resumed from CQE.user_data <---- operation_engine pump
        //
        // The kernel only sees the integer user_data. The actual operation
        // object, coroutine handle, keep-alive references and copied CQE result
        // stay in controller-owned memory.
        struct staging_buffer_wait_awaiter;
        struct operation_completion_awaiter;

        enum class lifecycle_state : uint8_t
        {
            running = 0,
            stopping = 1,
            stopped = 2
        };

        [[nodiscard]] int shutdown_error() const noexcept;
        [[nodiscard]] bool can_accept_work() const noexcept;
        CORO_TASK(operation_result) set_tcp_no_delay(uint32_t descriptor);
        void fail_staging_buffer_waiters(int error_code) noexcept;
        void fail_submission_waiters(int error_code) noexcept;
        void record_completion_pump(uint32_t completion_count) noexcept;
        void consume_pump_result(detail::direct_ring_completion_pump_result& pump_result) noexcept;
        void consume_submit_result(detail::direct_ring_submission_result& submit_result) noexcept;
        void resume_completed_operations(detail::direct_ring_operation_engine::operation_ptr completed_operations) noexcept;
        void record_no_op_complete(
            uint64_t start_ticks,
            int err) noexcept;

        [[nodiscard]] data cached_iouring_data_copy() const noexcept;
        [[nodiscard]] bool cached_fixed_file_table_available() const noexcept;
        CORO_TASK(int) ensure_iouring_data();
        CORO_TASK(int) ensure_fixed_file_table();

        int initialize_staging_buffer_cache_locked(const data& ring_data);
        CORO_TASK(staging_buffer_pair_allocation_result)
        allocate_staging_buffers(
            uint32_t required_buffer_count,
            size_t first_requested_size,
            size_t second_requested_size);
        CORO_TASK(staging_buffer_allocation_result) allocate_staging_buffer(size_t requested_size);
        CORO_TASK(staging_buffer_pair_allocation_result)
        allocate_staging_buffer_pair(
            size_t first_requested_size,
            size_t second_requested_size);
        [[nodiscard]] std::shared_ptr<staging_buffer_waiter> make_staging_buffer_waiter(
            uint32_t required_buffer_count,
            size_t first_requested_size,
            size_t second_requested_size,
            int& error_code) const;
        [[nodiscard]] int reserve_staging_buffers_locked(
            uint32_t required_buffer_count,
            size_t first_requested_size,
            size_t second_requested_size,
            staging_buffer_reservation& first_reservation,
            staging_buffer_reservation& second_reservation) noexcept;
        std::shared_ptr<staging_buffer_waiter> grant_next_staging_buffer_waiter_locked() noexcept;
        void cancel_staging_buffer_waiter(const std::shared_ptr<staging_buffer_waiter>& waiter) noexcept;
        void resume_staging_buffer_waiter(const std::shared_ptr<staging_buffer_waiter>& waiter) noexcept;
        [[nodiscard]] staging_buffer_pair_allocation_result make_staging_buffers_from_reservations(
            uint32_t required_buffer_count,
            const staging_buffer_reservation& first_reservation,
            const staging_buffer_reservation& second_reservation);
        void release_staging_buffer(
            uint32_t slot,
            uint64_t generation) noexcept;
        CORO_TASK(staging_buffer_allocation_result)
        make_ipv4_address_buffer(
            std::array<
                uint8_t,
                4> address,
            uint16_t port);
        CORO_TASK(staging_buffer_allocation_result)
        make_ipv6_address_buffer(
            std::array<
                uint8_t,
                16> address,
            uint16_t port);
        CORO_TASK(staging_buffer_allocation_result) make_loopback_address_buffer(uint16_t port);
        CORO_TASK(transfer_result)
        send_with_flags(
            uint32_t descriptor,
            rpc::byte_span buffer,
            uint32_t msg_flags);
        CORO_TASK(transfer_result)
        receive_with_flags(
            uint32_t descriptor,
            rpc::mutable_byte_span buffer,
            uint32_t msg_flags);

        CORO_TASK(operation_result)
        submit_operation(
            fill_sqe_callback fill_sqe,
            void* context);
        CORO_TASK(operation_result)
        submit_linked_operation(
            fill_linked_sqe_callback fill_sqes,
            void* context,
            std::shared_ptr<void> linked_keep_alive);
        CORO_TASK(int)
        submit_no_op(
            data ring_data,
            std::shared_ptr<detail::direct_ring_operation> operation);
        CORO_TASK(int)
        submit_prepared_operation(
            data ring_data,
            std::shared_ptr<detail::direct_ring_operation> operation,
            fill_sqe_callback fill_sqe,
            void* context);
        CORO_TASK(int)
        submit_prepared_linked_operation(
            data ring_data,
            std::shared_ptr<detail::direct_ring_operation> primary_operation,
            std::shared_ptr<detail::direct_ring_operation> linked_operation,
            fill_linked_sqe_callback fill_sqes,
            void* context);
        [[nodiscard]] std::shared_ptr<submission_waiter> make_submission_waiter(
            uint32_t required_sqe_count,
            int& error_code) const;
        bool can_attempt_submission(const std::shared_ptr<submission_waiter>& waiter);
        void complete_submission_waiter(const std::shared_ptr<submission_waiter>& waiter) noexcept;
        void cancel_submission_waiter(const std::shared_ptr<submission_waiter>& waiter) noexcept;

        CORO_TASK(int)
        wait_for_operation(
            data ring_data,
            std::shared_ptr<detail::direct_ring_operation> operation);
        CORO_TASK(int)
        wait_for_operation_cooperative(
            data ring_data,
            std::shared_ptr<detail::direct_ring_operation> operation);
        CORO_TASK(int)
        wait_for_operation_proactor(
            data ring_data,
            std::shared_ptr<detail::direct_ring_operation> operation);
        bool request_completion_pump(const data& ring_data) noexcept;
        CORO_TASK(void) completion_pump_loop(data ring_data);
        CORO_TASK(void) wait_before_next_poll();

        data cached_iouring_data_;
        mutable rpc::spin_mutex cache_mutex_;
        bool has_cached_iouring_data_{false};
        std::shared_ptr<io_uring_handle> handle_;
        detail::direct_ring_operation_engine operation_engine_;
        rpc::coro::scheduler* scheduler_{nullptr};
        options options_;
        wait_strategy wait_strategy_{wait_strategy::cooperative_poll};
        std::atomic<lifecycle_state> lifecycle_state_{lifecycle_state::running};
        std::atomic<int> shutdown_error_code_{rpc::error::OK()};
        std::atomic<bool> completion_pump_active_{false};
        rpc::spin_mutex staging_buffer_mutex_;
        // Bitmap for ring_data.buffers. A non-zero byte means the matching slot
        // is currently owned by a staging_buffer guard or by a granted waiter
        // that has not yet converted its reservation into a guard.
        std::vector<uint8_t> staging_buffer_slots_in_use_;
        std::deque<std::shared_ptr<staging_buffer_waiter>> staging_buffer_waiters_;
        rpc::spin_mutex submission_waiter_mutex_;
        std::deque<std::shared_ptr<submission_waiter>> submission_waiters_;
        uint64_t buffer_region_ptr_{0};
        uint32_t staging_buffer_count_{0};
        uint32_t staging_buffer_size_{0};
        uint64_t staging_buffer_generation_{0};
        atomic_measurements measurements_;
    };
} // namespace rpc::io_uring
