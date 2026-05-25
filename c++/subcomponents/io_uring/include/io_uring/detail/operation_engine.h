/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>
#include <utility>

#include <io_uring/types.h>
#include <rpc/rpc.h>

#ifdef FOR_SGX
#  include <sgx_trts.h>
#endif

namespace rpc::io_uring::detail
{
    struct command_op_fields
    {
        uint32_t cmd_op{0};
        uint32_t pad{0};
    };

    struct socket_option_name_fields
    {
        uint32_t level{0};
        uint32_t optname{0};
    };

    // The direct-ring controller writes SQEs and drains CQEs through a normalized
    // descriptor that may come from host Linux or from an enclave bridge. Keep
    // the fixed kernel ABI layout here instead of exposing liburing types through
    // the common controller surface.
    struct sqe_64
    {
        uint8_t opcode{0};
        uint8_t flags{0};
        uint16_t ioprio{0};
        int32_t fd{0};
        union
        {
            uint64_t off;
            uint64_t addr2;
            command_op_fields command;
        };
        union
        {
            uint64_t addr;
            uint64_t splice_off_in;
            socket_option_name_fields socket_option;
        };
        uint32_t len{0};
        union
        {
            uint32_t rw_flags;
            uint32_t msg_flags;
            uint32_t accept_flags;
            uint32_t open_flags;
            uint32_t statx_flags;
            uint32_t splice_flags;
            uint32_t nop_flags;
        };
        uint64_t user_data{0};
        union
        {
            uint16_t buf_index;
            uint16_t buf_group;
        };
        uint16_t personality{0};
        union
        {
            int32_t splice_fd_in;
            uint32_t file_index;
            uint32_t addr_len;
            uint32_t optlen;
        };
        union
        {
            uint64_t addr3;
            uint64_t optval;
        };
        uint64_t pad2{0};
    };

    struct cqe_16
    {
        uint64_t user_data{0};
        int32_t res{0};
        uint32_t flags{0};
    };

    struct kernel_timespec
    {
        int64_t tv_sec{0};
        int64_t tv_nsec{0};
    };

    static_assert(sizeof(sqe_64) == 64);
    static_assert(sizeof(cqe_16) == 16);
    static_assert(sizeof(kernel_timespec) == 16);
    static_assert(sizeof(command_op_fields) == sizeof(uint64_t));
    static_assert(sizeof(socket_option_name_fields) == sizeof(uint64_t));

    template<class T> static T* ring_pointer(uint64_t address) noexcept
    {
        return reinterpret_cast<T*>(static_cast<std::uintptr_t>(address));
    }

    static inline uint32_t load_ring_u32_acquire(const uint32_t* value) noexcept
    {
        auto result = *reinterpret_cast<const volatile uint32_t*>(value);
        std::atomic_thread_fence(std::memory_order_acquire);
        return result;
    }

    static inline void store_ring_u32_release(
        uint32_t* target,
        uint32_t value) noexcept
    {
        std::atomic_thread_fence(std::memory_order_release);
        *reinterpret_cast<volatile uint32_t*>(target) = value;
    }

    static inline bool checked_size(
        uint32_t count,
        size_t item_size,
        size_t& size) noexcept
    {
        if (item_size != 0 && static_cast<size_t>(count) > std::numeric_limits<size_t>::max() / item_size)
        {
            return false;
        }

        size = static_cast<size_t>(count) * item_size;
        return true;
    }

    static inline bool is_accessible_ring_pointer(
        uint64_t address,
        size_t size) noexcept
    {
        if (address == 0 || size == 0)
        {
            return false;
        }

        const auto* address_ptr = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(address));
#if defined(FOR_SGX) && !defined(CANOPY_FAKE_SGX)
        return sgx_is_outside_enclave(address_ptr, size) != 0;
#else
        // Fake SGX tracks ECALL argument buffers, but the io_uring rings are
        // long-lived host/kernel mmap regions discovered through RPC. Treat
        // non-null ring pointers as host memory here; real SGX still uses the
        // hardware/runtime outside-enclave predicate above.
        (void)address_ptr;
        return true;
#endif
    }

    static constexpr uint8_t io_uring_op_nop = 0;
    static constexpr uint8_t io_uring_op_accept = 13;
    static constexpr uint8_t io_uring_op_async_cancel = 14;
    static constexpr uint8_t io_uring_op_link_timeout = 15;
    static constexpr uint8_t io_uring_op_connect = 16;
    static constexpr uint8_t io_uring_op_openat = 18;
    static constexpr uint8_t io_uring_op_close = 19;
    static constexpr uint8_t io_uring_op_statx = 21;
    static constexpr uint8_t io_uring_op_read = 22;
    static constexpr uint8_t io_uring_op_write = 23;
    static constexpr uint8_t io_uring_op_send = 26;
    static constexpr uint8_t io_uring_op_recv = 27;
    static constexpr uint8_t io_uring_op_socket = 45;
    static constexpr uint8_t io_uring_op_uring_cmd = 46;
    static constexpr uint8_t io_uring_op_bind = 56;
    static constexpr uint8_t io_uring_op_listen = 57;
    static constexpr uint8_t io_uring_sqe_fixed_file = 1U << 0;
    static constexpr uint8_t io_uring_sqe_io_link = 1U << 2;
    static constexpr uint32_t io_uring_file_index_alloc = ~0U;
    static constexpr uint32_t io_uring_async_cancel_all = 1U << 0;
    static constexpr uint32_t io_uring_async_cancel_fd = 1U << 1;
    static constexpr uint32_t io_uring_async_cancel_fd_fixed = 1U << 3;
    static constexpr uint32_t socket_uring_op_setsockopt = 3;
    static constexpr uint32_t io_uring_sq_need_wakeup = 1U << 0;
    static constexpr uint32_t io_uring_setup_sqpoll = 1U << 1;
    static constexpr uint32_t io_uring_setup_no_sqarray = 1U << 16;

    static inline bool has_no_sqarray(const data& ring_data) noexcept
    {
        return (ring_data.setup.setup_flags & io_uring_setup_no_sqarray) != 0;
    }

    static inline bool has_sqpoll(const data& ring_data) noexcept
    {
        return (ring_data.setup.setup_flags & io_uring_setup_sqpoll) != 0;
    }

    static inline bool sqpoll_needs_wakeup(const data& ring_data) noexcept
    {
        auto* sq_flags = ring_pointer<uint32_t>(ring_data.submission_queue.sq_flags_ptr);
        return (load_ring_u32_acquire(sq_flags) & io_uring_sq_need_wakeup) != 0;
    }

    static inline bool submission_notification_needed(const data& ring_data) noexcept
    {
        return !has_sqpoll(ring_data) || sqpoll_needs_wakeup(ring_data);
    }

    static inline bool validate_ring_data_for_direct_ring(const data& ring_data) noexcept
    {
        const auto& sq = ring_data.submission_queue;
        const auto& cq = ring_data.completion_queue;
        const auto uses_sq_array = !has_no_sqarray(ring_data);

        if ((ring_data.descriptor_version != 1 && ring_data.descriptor_version != 2) || sq.sq_ring_entries == 0
            || cq.cq_ring_entries == 0)
        {
            return false;
        }

        size_t sqes_size = 0;
        size_t sq_array_size = 0;
        size_t cqes_size = 0;
        if (!checked_size(sq.sq_ring_entries, sizeof(sqe_64), sqes_size)
            || !checked_size(sq.sq_ring_entries, sizeof(uint32_t), sq_array_size)
            || !checked_size(cq.cq_ring_entries, sizeof(cqe_16), cqes_size))
        {
            return false;
        }

        return is_accessible_ring_pointer(sq.sq_head_ptr, sizeof(uint32_t))
               && is_accessible_ring_pointer(sq.sq_tail_ptr, sizeof(uint32_t))
               && is_accessible_ring_pointer(sq.sq_flags_ptr, sizeof(uint32_t))
               && (!uses_sq_array || is_accessible_ring_pointer(sq.sq_array_ptr, sq_array_size))
               && is_accessible_ring_pointer(sq.sqes_ptr, sqes_size)
               && is_accessible_ring_pointer(cq.cq_head_ptr, sizeof(uint32_t))
               && is_accessible_ring_pointer(cq.cq_tail_ptr, sizeof(uint32_t))
               && is_accessible_ring_pointer(cq.cqes_ptr, cqes_size);
    }

    // Controller-owned state for one submitted operation.
    //
    // user_data is the only value written into the untrusted SQE. CQ dispatch
    // copies the CQE into controller-owned memory, finds this operation by
    // user_data, and then publishes completion with release ordering.
    struct direct_ring_operation
    {
        std::atomic<bool> submitted{false};
        std::atomic<bool> completed{false};
        std::coroutine_handle<> waiter{};
        std::shared_ptr<direct_ring_operation> next_completed;
        std::shared_ptr<void> keep_alive;
        uint64_t user_data{0};
        int32_t cqe_result{0};
        uint32_t cqe_flags{0};
        int error_code{rpc::error::OK()};
    };

    struct direct_ring_submission_result
    {
        int error_code{rpc::error::OK()};
        bool submitted{false};
        uint32_t completion_count{0};
        std::shared_ptr<direct_ring_operation> completed_operations;
    };

    struct direct_ring_completion_pump_result
    {
        int error_code{rpc::error::OK()};
        uint32_t completion_count{0};
        std::shared_ptr<direct_ring_operation> completed_operations;
    };

    class direct_ring_operation_engine
    {
    public:
        using operation_ptr = std::shared_ptr<direct_ring_operation>;

        direct_ring_submission_result try_submit_no_op(
            const data& ring_data,
            const operation_ptr& operation)
        {
            return try_submit(ring_data, operation, [](sqe_64& sqe) { sqe.opcode = io_uring_op_nop; });
        }

        template<class FillSqe>
        direct_ring_submission_result try_submit(
            const data& ring_data,
            const operation_ptr& operation,
            FillSqe&& fill_sqe)
        {
            std::lock_guard<rpc::spin_mutex> lock(mutex_);
            if (controller_error_ != rpc::error::OK())
            {
                return {controller_error_, false, 0, {}};
            }

            // Opportunistically drain completions before checking capacity. This
            // lets a burst of more operations than the ring depth make progress
            // without a separate background poller.
            auto completion_count = pump_completions_locked(ring_data);
            if (controller_error_ != rpc::error::OK())
            {
                return {controller_error_, false, completion_count, std::move(completed_operations_)};
            }

            if (operations_.size() >= ring_data.completion_queue.cq_ring_entries)
            {
                return {rpc::error::OK(), false, completion_count, std::move(completed_operations_)};
            }

            auto& sq = ring_data.submission_queue;
            auto* sq_head = ring_pointer<uint32_t>(sq.sq_head_ptr);
            auto* sq_tail = ring_pointer<uint32_t>(sq.sq_tail_ptr);
            auto* sq_array = ring_pointer<uint32_t>(sq.sq_array_ptr);
            auto* sqes = ring_pointer<sqe_64>(sq.sqes_ptr);

            const auto head = load_ring_u32_acquire(sq_head);
            const auto tail = load_ring_u32_acquire(sq_tail);
            if ((tail - head) >= sq.sq_ring_entries)
            {
                return {rpc::error::OK(), false, completion_count, std::move(completed_operations_)};
            }

            const auto user_data = next_user_data_++;
            operation->user_data = user_data;

            try
            {
                operations_.emplace(user_data, operation);
            }
            catch (const std::bad_alloc&)
            {
                RPC_ERROR("bad_alloc while registering direct io_uring operation");
                std::terminate();
            }

            const auto sq_index = tail & sq.sq_ring_mask;
            auto& sqe = sqes[sq_index];
            std::memset(&sqe, 0, sizeof(sqe));
            std::forward<FillSqe>(fill_sqe)(sqe);

            sqe.user_data = user_data;
            if (!has_no_sqarray(ring_data))
            {
                sq_array[sq_index] = sq_index;
            }

            // Publish the SQE only after it and the SQ array entry are complete.
            // The kernel-side SQPOLL thread observes this tail with acquire
            // ordering.
            store_ring_u32_release(sq_tail, tail + 1);
            operation->submitted.store(true, std::memory_order_release);
            return {rpc::error::OK(), true, completion_count, std::move(completed_operations_)};
        }

        template<class FillSqes>
        direct_ring_submission_result try_submit_linked(
            const data& ring_data,
            const operation_ptr& primary_operation,
            const operation_ptr& linked_operation,
            FillSqes&& fill_sqes)
        {
            std::lock_guard<rpc::spin_mutex> lock(mutex_);
            if (controller_error_ != rpc::error::OK())
            {
                return {controller_error_, false, 0, {}};
            }

            auto completion_count = pump_completions_locked(ring_data);
            if (controller_error_ != rpc::error::OK())
            {
                return {controller_error_, false, completion_count, std::move(completed_operations_)};
            }

            if (operations_.size() + 2 > ring_data.completion_queue.cq_ring_entries)
            {
                return {rpc::error::OK(), false, completion_count, std::move(completed_operations_)};
            }

            auto& sq = ring_data.submission_queue;
            auto* sq_head = ring_pointer<uint32_t>(sq.sq_head_ptr);
            auto* sq_tail = ring_pointer<uint32_t>(sq.sq_tail_ptr);
            auto* sq_array = ring_pointer<uint32_t>(sq.sq_array_ptr);
            auto* sqes = ring_pointer<sqe_64>(sq.sqes_ptr);

            const auto head = load_ring_u32_acquire(sq_head);
            const auto tail = load_ring_u32_acquire(sq_tail);
            if ((tail - head) + 2 > sq.sq_ring_entries)
            {
                return {rpc::error::OK(), false, completion_count, std::move(completed_operations_)};
            }

            const auto primary_user_data = next_user_data_++;
            const auto linked_user_data = next_user_data_++;
            primary_operation->user_data = primary_user_data;
            linked_operation->user_data = linked_user_data;

            try
            {
                operations_.emplace(primary_user_data, primary_operation);
                operations_.emplace(linked_user_data, linked_operation);
            }
            catch (const std::bad_alloc&)
            {
                RPC_ERROR("bad_alloc while registering linked direct io_uring operations");
                std::terminate();
            }

            const auto primary_sq_index = tail & sq.sq_ring_mask;
            const auto linked_sq_index = (tail + 1) & sq.sq_ring_mask;
            auto& primary_sqe = sqes[primary_sq_index];
            auto& linked_sqe = sqes[linked_sq_index];
            std::memset(&primary_sqe, 0, sizeof(primary_sqe));
            std::memset(&linked_sqe, 0, sizeof(linked_sqe));
            std::forward<FillSqes>(fill_sqes)(primary_sqe, linked_sqe);

            primary_sqe.user_data = primary_user_data;
            linked_sqe.user_data = linked_user_data;
            if (!has_no_sqarray(ring_data))
            {
                sq_array[primary_sq_index] = primary_sq_index;
                sq_array[linked_sq_index] = linked_sq_index;
            }

            // Publish both SQEs with a single tail update so the kernel observes
            // the linked pair in order.
            store_ring_u32_release(sq_tail, tail + 2);
            primary_operation->submitted.store(true, std::memory_order_release);
            linked_operation->submitted.store(true, std::memory_order_release);
            return {rpc::error::OK(), true, completion_count, std::move(completed_operations_)};
        }

        direct_ring_completion_pump_result pump_completions(const data& ring_data) noexcept
        {
            std::lock_guard<rpc::spin_mutex> lock(mutex_);
            if (controller_error_ != rpc::error::OK())
            {
                return {controller_error_, 0, std::move(completed_operations_)};
            }

            auto completion_count = pump_completions_locked(ring_data);
            return {controller_error_, completion_count, std::move(completed_operations_)};
        }

        std::shared_ptr<direct_ring_operation> fail_all_operations(int error_code) noexcept
        {
            std::lock_guard<rpc::spin_mutex> lock(mutex_);
            fail_all_operations_locked(error_code);
            controller_error_ = error_code;
            return std::move(completed_operations_);
        }

        bool register_waiter(
            const operation_ptr& operation,
            std::coroutine_handle<> waiter) noexcept
        {
            std::lock_guard<rpc::spin_mutex> lock(mutex_);
            if (!operation || !waiter || waiter.done())
            {
                return false;
            }

            if (controller_error_ != rpc::error::OK())
            {
                operation->error_code = controller_error_;
                operation->completed.store(true, std::memory_order_release);
                return false;
            }

            if (operation->completed.load(std::memory_order_acquire))
            {
                return false;
            }

            operation->waiter = waiter;
            return true;
        }

        bool has_pending_operations() noexcept
        {
            std::lock_guard<rpc::spin_mutex> lock(mutex_);
            return !operations_.empty();
        }

    private:
        void append_completed_operation_locked(const operation_ptr& operation) noexcept
        {
            if (!operation || !operation->waiter)
            {
                return;
            }

            operation->next_completed = std::move(completed_operations_);
            completed_operations_ = operation;
        }

        uint32_t pump_completions_locked(const data& ring_data) noexcept
        {
            auto& cq = ring_data.completion_queue;
            auto* cq_head = ring_pointer<uint32_t>(cq.cq_head_ptr);
            auto* cq_tail = ring_pointer<uint32_t>(cq.cq_tail_ptr);
            auto* cqes = ring_pointer<cqe_16>(cq.cqes_ptr);

            auto head = load_ring_u32_acquire(cq_head);
            const auto tail = load_ring_u32_acquire(cq_tail);
            uint32_t completion_count = 0;
            while (head != tail)
            {
                // CQ memory may be shared with another trust domain. Copy the
                // entry into controller-owned stack memory before looking at
                // user_data or result fields.
                const auto cqe = cqes[head & cq.cq_ring_mask];
                auto op = operations_.find(cqe.user_data);
                if (op == operations_.end())
                {
                    RPC_WARNING(
                        "direct io_uring unknown completion user_data={} cqe_res={} cq_head={} cq_tail={}",
                        cqe.user_data,
                        cqe.res,
                        head,
                        tail);
                    fail_all_operations_locked(rpc::error::PROTOCOL_ERROR());
                    controller_error_ = rpc::error::PROTOCOL_ERROR();
                }
                else
                {
                    auto operation = op->second;
                    operations_.erase(op);
                    operation->cqe_result = cqe.res;
                    operation->cqe_flags = cqe.flags;
                    operation->error_code = cqe.res >= 0 ? rpc::error::OK() : rpc::error::NATIVE_IO_ERROR();
                    operation->completed.store(true, std::memory_order_release);
                    append_completed_operation_locked(operation);
                }
                ++head;
                ++completion_count;
            }

            // Publish CQ consumption after all copied completions have been
            // routed to controller-owned operation state.
            store_ring_u32_release(cq_head, head);
            return completion_count;
        }

        void fail_all_operations_locked(int error_code) noexcept
        {
            for (auto& [_, operation] : operations_)
            {
                operation->error_code = error_code;
                operation->completed.store(true, std::memory_order_release);
                append_completed_operation_locked(operation);
            }
            operations_.clear();
        }

        rpc::spin_mutex mutex_;
        std::unordered_map<uint64_t, operation_ptr> operations_;
        operation_ptr completed_operations_;
        uint64_t next_user_data_{1};
        int controller_error_{rpc::error::OK()};
    };
} // namespace rpc::io_uring::detail
