/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <utility>
#include <vector>

#include <io_uring/detail/operation_engine.h>
#include <io_uring/types.h>
#include <edl/coroutine_enclave.h>
#include <rpc/rpc.h>

namespace rpc::io_uring
{
    class direct_descriptor;

    class controller : public std::enable_shared_from_this<controller>
    {
    public:
        explicit controller(rpc::coro::scheduler* scheduler = nullptr);
        virtual ~controller() = default;

        [[nodiscard]] rpc::optimistic_ptr<i_io_uring_control> host_control() const;

        void set_wait_strategy(wait_strategy strategy) noexcept;
        [[nodiscard]] wait_strategy get_wait_strategy() const noexcept;

        // Only reset measurements when the caller knows no operations from the
        // previous sample are still in flight.
        void reset_measurements() noexcept;
        [[nodiscard]] controller_measurements measurements() const noexcept;

        CORO_TASK(int) shutdown();
        void request_shutdown() noexcept;
        void request_shutdown(int error_code) noexcept;
        [[nodiscard]] bool is_shutdown_requested() const noexcept;

        CORO_TASK(int) wake_host_iouring();
        CORO_TASK(int) get_iouring_data(data& ring_data);
        CORO_TASK(int) refresh_iouring_data();
        void clear_iouring_data_cache() noexcept;
        [[nodiscard]] const data* cached_iouring_data() const noexcept;

        CORO_TASK(int) no_op();

        CORO_TASK(descriptor_result) create_tcp_socket();
        CORO_TASK(operation_result)
        bind_tcp_ipv4_loopback(
            uint32_t descriptor,
            uint16_t port);
        CORO_TASK(operation_result)
        listen(
            uint32_t descriptor,
            uint32_t backlog);
        CORO_TASK(descriptor_result) accept(uint32_t listen_descriptor);
        CORO_TASK(descriptor_result) connect_tcp_ipv4_loopback(uint16_t port);
        CORO_TASK(transfer_result)
        send(
            uint32_t descriptor,
            rpc::byte_span buffer);
        CORO_TASK(transfer_result)
        receive(
            uint32_t descriptor,
            rpc::mutable_byte_span buffer);
        CORO_TASK(transfer_result)
        receive(
            uint32_t descriptor,
            rpc::mutable_byte_span buffer,
            std::chrono::milliseconds timeout);
        CORO_TASK(operation_result) cancel_direct(uint32_t descriptor);
        CORO_TASK(operation_result) close_direct(uint32_t descriptor);

    protected:
        virtual CORO_TASK(int) inner_wake_host_iouring() = 0;
        virtual CORO_TASK(int) inner_get_iouring_data(rpc::io_uring::data& ring_data) = 0;

    private:
        friend class direct_descriptor;

        class host_buffer
        {
        public:
            ~host_buffer();

            host_buffer(const host_buffer&) = delete;
            host_buffer& operator=(const host_buffer&) = delete;
            host_buffer(host_buffer&&) = delete;
            host_buffer& operator=(host_buffer&&) = delete;

            [[nodiscard]] uint8_t* data() const noexcept;
            [[nodiscard]] size_t size() const noexcept;
            [[nodiscard]] uint64_t address() const noexcept;

        private:
            friend class controller;

            host_buffer(
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

        struct host_buffer_allocation_result
        {
            int error_code{rpc::error::OK()};
            std::shared_ptr<host_buffer> buffer;
        };

        struct host_buffer_pair_allocation_result
        {
            int error_code{rpc::error::OK()};
            std::shared_ptr<host_buffer> first_buffer;
            std::shared_ptr<host_buffer> second_buffer;
        };

        struct host_buffer_reservation
        {
            uint32_t slot{0};
            uint64_t generation{0};
            uint8_t* data{nullptr};
            size_t size{0};
        };

        struct host_buffer_waiter
        {
            uint32_t required_buffer_count{0};
            size_t first_requested_size{0};
            size_t second_requested_size{0};
            bool queued{false};
            bool granted{false};
            int error_code{rpc::error::OK()};
            host_buffer_reservation first_reservation;
            host_buffer_reservation second_reservation;
        };

        struct submission_waiter
        {
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

        struct operation_completion_awaiter;

        enum class lifecycle_state : uint8_t
        {
            running = 0,
            stopping = 1,
            stopped = 2
        };

        [[nodiscard]] int shutdown_error() const noexcept;
        [[nodiscard]] bool can_accept_work() const noexcept;
        void fail_host_buffer_waiters(int error_code) noexcept;
        void fail_submission_waiters(int error_code) noexcept;
        void record_completion_pump(uint32_t completion_count) noexcept;
        void consume_pump_result(detail::enclave_io_completion_pump_result& pump_result) noexcept;
        void consume_submit_result(detail::enclave_io_submission_result& submit_result) noexcept;
        void resume_completed_operations(detail::enclave_io_operation_engine::operation_ptr completed_operations) noexcept;
        void record_no_op_complete(
            uint64_t start_ticks,
            int err) noexcept;

        [[nodiscard]] data cached_iouring_data_copy() const noexcept;
        CORO_TASK(int) ensure_iouring_data();
        CORO_TASK(int) ensure_fixed_file_table();

        int initialize_host_buffer_cache_locked(const data& ring_data);
        CORO_TASK(host_buffer_pair_allocation_result)
        allocate_host_buffers(
            uint32_t required_buffer_count,
            size_t first_requested_size,
            size_t second_requested_size);
        CORO_TASK(host_buffer_allocation_result) allocate_host_buffer(size_t requested_size);
        CORO_TASK(host_buffer_pair_allocation_result)
        allocate_host_buffer_pair(
            size_t first_requested_size,
            size_t second_requested_size);
        [[nodiscard]] std::shared_ptr<host_buffer_waiter> make_host_buffer_waiter(
            uint32_t required_buffer_count,
            size_t first_requested_size,
            size_t second_requested_size,
            int& error_code) const;
        [[nodiscard]] int reserve_host_buffers_locked(
            uint32_t required_buffer_count,
            size_t first_requested_size,
            size_t second_requested_size,
            host_buffer_reservation& first_reservation,
            host_buffer_reservation& second_reservation) noexcept;
        std::shared_ptr<host_buffer_waiter> grant_next_host_buffer_waiter_locked() noexcept;
        void cancel_host_buffer_waiter(const std::shared_ptr<host_buffer_waiter>& waiter) noexcept;
        [[nodiscard]] host_buffer_pair_allocation_result make_host_buffers_from_reservations(
            uint32_t required_buffer_count,
            const host_buffer_reservation& first_reservation,
            const host_buffer_reservation& second_reservation);
        void release_host_buffer(
            uint32_t slot,
            uint64_t generation) noexcept;
        CORO_TASK(host_buffer_allocation_result) make_loopback_address_buffer(uint16_t port);

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
            const data& ring_data,
            const std::shared_ptr<detail::enclave_io_operation>& operation);
        CORO_TASK(int)
        submit_prepared_operation(
            const data& ring_data,
            const std::shared_ptr<detail::enclave_io_operation>& operation,
            fill_sqe_callback fill_sqe,
            void* context);
        CORO_TASK(int)
        submit_prepared_linked_operation(
            const data& ring_data,
            const std::shared_ptr<detail::enclave_io_operation>& primary_operation,
            const std::shared_ptr<detail::enclave_io_operation>& linked_operation,
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
            const data& ring_data,
            const std::shared_ptr<detail::enclave_io_operation>& operation);
        CORO_TASK(int)
        wait_for_operation_cooperative(
            const data& ring_data,
            const std::shared_ptr<detail::enclave_io_operation>& operation);
        CORO_TASK(int)
        wait_for_operation_proactor(
            const data& ring_data,
            const std::shared_ptr<detail::enclave_io_operation>& operation);
        bool request_completion_pump(const data& ring_data) noexcept;
        CORO_TASK(void) completion_pump_loop(data ring_data);
        CORO_TASK(void) wait_before_next_poll();

        data cached_iouring_data_;
        mutable rpc::spin_mutex cache_mutex_;
        bool has_cached_iouring_data_{false};
        detail::enclave_io_operation_engine operation_engine_;
        rpc::coro::scheduler* scheduler_{nullptr};
        wait_strategy wait_strategy_{wait_strategy::cooperative_poll};
        std::atomic<lifecycle_state> lifecycle_state_{lifecycle_state::running};
        std::atomic<int> shutdown_error_code_{rpc::error::OK()};
        std::atomic<bool> completion_pump_active_{false};
        rpc::spin_mutex host_buffer_mutex_;
        std::vector<uint8_t> host_buffer_slots_in_use_;
        std::deque<std::shared_ptr<host_buffer_waiter>> host_buffer_waiters_;
        rpc::spin_mutex submission_waiter_mutex_;
        std::deque<std::shared_ptr<submission_waiter>> submission_waiters_;
        uint64_t buffer_region_ptr_{0};
        uint32_t host_buffer_count_{0};
        uint32_t host_buffer_size_{0};
        uint64_t host_buffer_generation_{0};
        atomic_measurements measurements_;
    };
} // namespace rpc::io_uring
