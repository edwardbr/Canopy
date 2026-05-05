/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <io_uring/host_controller.h>

#include <coro/coro.hpp>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <liburing.h>
#include <linux/io_uring.h>
#include <new>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace rpc::io_uring
{
    struct host_controller::state
    {
        ::io_uring ring{};
        io_uring_params params{};
        options controller_options{};
        std::vector<uint8_t> buffer_region;
        std::vector<iovec> registered_buffers;
        bool buffers_registered{false};
        bool fixed_files_registered{false};
        uint32_t fixed_file_count{0};
        std::atomic<bool> open{false};
        std::mutex mutex;
    };

    namespace
    {
        auto wake_sqpoll_thread(int ring_fd) noexcept -> int
        {
            return static_cast<int>(::syscall(__NR_io_uring_enter, ring_fd, 0U, 0U, IORING_ENTER_SQ_WAKEUP, nullptr, 0U));
        }

        auto pointer_value(const void* ptr) noexcept -> uint64_t
        {
            return static_cast<uint64_t>(reinterpret_cast<std::uintptr_t>(ptr));
        }

        auto setup_flags(const host_controller::options& controller_options) noexcept -> uint32_t
        {
            uint32_t flags = 0;
            if (controller_options.use_sqpoll)
            {
                flags |= IORING_SETUP_SQPOLL;
                if (controller_options.use_single_issuer)
                {
                    flags |= IORING_SETUP_SINGLE_ISSUER;
                }
            }
            else if (controller_options.use_coop_taskrun)
            {
                // COOP_TASKRUN is only meaningful for the non-SQPOLL path here.
                // With SQPOLL enabled, the kernel polling thread is responsible
                // for submission progress and is woken explicitly when needed.
                // COOP_TASKRUN asks the kernel to defer io_uring task-work until the
                // userspace submitter next enters the kernel, avoiding an interrupt/reschedule.
                // That is useful for some non-SQPOLL rings, but the SGX path uses SQPOLL
                // because the enclave cannot call io_uring_enter() directly.
                flags |= IORING_SETUP_COOP_TASKRUN;
            }
            return flags;
        }

        auto checked_buffer_region_size(
            uint32_t buffer_count,
            uint32_t buffer_size,
            size_t& output_size) noexcept -> bool
        {
            const auto count = static_cast<size_t>(buffer_count);
            const auto size = static_cast<size_t>(buffer_size);
            if (count != 0 && size > std::numeric_limits<size_t>::max() / count)
            {
                return false;
            }
            output_size = count * size;
            return true;
        }
    } // namespace

    host_controller::host_controller(
        options controller_options,
        std::shared_ptr<coro::scheduler> scheduler,
        std::shared_ptr<state> state) noexcept
        : options_(controller_options)
        , scheduler_(std::move(scheduler))
        , state_(std::move(state))
    {
    }

    int host_controller::create(
        std::unique_ptr<host_controller>& controller,
        options controller_options,
        std::shared_ptr<coro::scheduler> scheduler) noexcept
    {
        controller.reset();

        size_t buffer_region_size = 0;
        if (!checked_buffer_region_size(controller_options.buffer_count, controller_options.buffer_size, buffer_region_size))
        {
            RPC_WARNING(
                "io_uring buffer region size overflow buffer_count={} buffer_size={}",
                controller_options.buffer_count,
                controller_options.buffer_size);
            return rpc::error::INVALID_DATA();
        }

        std::shared_ptr<state> new_state;
        try
        {
            new_state = std::make_shared<state>();
            new_state->controller_options = controller_options;
            new_state->buffer_region.resize(buffer_region_size);
        }
        catch (const std::bad_alloc&)
        {
            return rpc::error::OUT_OF_MEMORY();
        }
        catch (...)
        {
            return rpc::error::EXCEPTION();
        }

        io_uring_params params{};
        params.flags = setup_flags(controller_options);
        if (controller_options.use_sqpoll)
        {
            params.sq_thread_idle = controller_options.sq_thread_idle_ms;
        }

        const auto result = io_uring_queue_init_params(controller_options.queue_depth, &new_state->ring, &params);
        if (result < 0)
        {
            RPC_WARNING("io_uring_queue_init_params failed errno_code={}", -result);
            return rpc::error::NATIVE_IO_ERROR();
        }
        new_state->params = params;
        new_state->open.store(true, std::memory_order_release);

        if (controller_options.use_sqpoll && wake_sqpoll_thread(new_state->ring.ring_fd) < 0)
        {
            RPC_WARNING("io_uring initial SQPOLL wake failed for ring_fd={} errno={}", new_state->ring.ring_fd, errno);
            close_state_now(new_state);
            return rpc::error::NATIVE_IO_ERROR();
        }

        if (controller_options.register_buffers && !new_state->buffer_region.empty())
        {
            try
            {
                new_state->registered_buffers.reserve(controller_options.buffer_count);
                auto* buffer_begin = new_state->buffer_region.data();
                for (uint32_t index = 0; index < controller_options.buffer_count; ++index)
                {
                    iovec iov{};
                    iov.iov_base = buffer_begin + (static_cast<size_t>(index) * controller_options.buffer_size);
                    iov.iov_len = controller_options.buffer_size;
                    new_state->registered_buffers.push_back(iov);
                }
            }
            catch (const std::bad_alloc&)
            {
                close_state_now(new_state);
                return rpc::error::OUT_OF_MEMORY();
            }
            catch (...)
            {
                close_state_now(new_state);
                return rpc::error::EXCEPTION();
            }

            const auto registration_result = io_uring_register_buffers(
                &new_state->ring,
                new_state->registered_buffers.data(),
                static_cast<unsigned int>(new_state->registered_buffers.size()));
            if (registration_result < 0)
            {
                RPC_WARNING("io_uring_register_buffers failed errno_code={}", -registration_result);
                close_state_now(new_state);
                return rpc::error::NATIVE_IO_ERROR();
            }
            new_state->buffers_registered = true;
        }

        if (controller_options.register_fixed_files && controller_options.fixed_file_count != 0)
        {
            const auto registration_result
                = io_uring_register_files_sparse(&new_state->ring, controller_options.fixed_file_count);
            if (registration_result < 0)
            {
                RPC_WARNING("io_uring_register_files_sparse failed errno_code={}", -registration_result);
                close_state_now(new_state);
                return rpc::error::NATIVE_IO_ERROR();
            }

            // This range is a policy hint for direct descriptor allocation. Some
            // older kernels accept sparse files but not this narrowing call; the
            // sparse table itself is still enough for direct descriptors, so do
            // not fail controller creation just because the hint was rejected.
            const auto alloc_range_result
                = io_uring_register_file_alloc_range(&new_state->ring, 0, controller_options.fixed_file_count);
            if (alloc_range_result < 0)
            {
                RPC_WARNING("io_uring_register_file_alloc_range failed errno_code={}", -alloc_range_result);
            }

            new_state->fixed_files_registered = true;
            new_state->fixed_file_count = controller_options.fixed_file_count;
        }

        try
        {
            controller.reset(new host_controller(controller_options, std::move(scheduler), std::move(new_state)));
        }
        catch (const std::bad_alloc&)
        {
            close_state_now(new_state);
            return rpc::error::OUT_OF_MEMORY();
        }
        catch (...)
        {
            close_state_now(new_state);
            return rpc::error::EXCEPTION();
        }

        return rpc::error::OK();
    }

    host_controller::~host_controller()
    {
        close();
    }

    int host_controller::wake_iouring() noexcept
    {
        auto state = get_state();
        if (!state)
        {
            return rpc::error::RESOURCE_CLOSED();
        }

        if (!state->controller_options.use_sqpoll)
        {
            return rpc::error::OK();
        }

        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->open.load(std::memory_order_acquire))
        {
            return rpc::error::RESOURCE_CLOSED();
        }

        if (wake_sqpoll_thread(state->ring.ring_fd) < 0)
        {
            RPC_WARNING("io_uring SQPOLL wake failed for ring_fd={} errno={}", state->ring.ring_fd, errno);
            return rpc::error::NATIVE_IO_ERROR();
        }

        return rpc::error::OK();
    }

    int host_controller::get_iouring_data(data& ring_data) noexcept
    {
        auto state = get_state();
        if (!state)
        {
            return rpc::error::RESOURCE_CLOSED();
        }

        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->open.load(std::memory_order_acquire))
        {
            return rpc::error::RESOURCE_CLOSED();
        }

        const auto& ring = state->ring;
        const auto& params = state->params;
        const auto& sq = ring.sq;
        const auto& cq = ring.cq;

        ring_data.descriptor_version = 2;
        ring_data.file_descriptors.has_ring_fd = ring.ring_fd >= 0;
        ring_data.file_descriptors.ring_fd = ring.ring_fd;
        ring_data.file_descriptors.has_enter_ring_fd = ring.enter_ring_fd >= 0;
        ring_data.file_descriptors.enter_ring_fd = ring.enter_ring_fd;

        ring_data.setup.setup_flags = ring.flags;
        ring_data.setup.features = ring.features;
        ring_data.setup.sq_thread_idle_ms = params.sq_thread_idle;
        ring_data.setup.sq_entries = params.sq_entries;
        ring_data.setup.cq_entries = params.cq_entries;

        ring_data.submission_queue.sq_ring_mask = sq.ring_mask;
        ring_data.submission_queue.sq_ring_entries = sq.ring_entries;
        ring_data.submission_queue.sq_off_head = params.sq_off.head;
        ring_data.submission_queue.sq_off_tail = params.sq_off.tail;
        ring_data.submission_queue.sq_off_ring_mask = params.sq_off.ring_mask;
        ring_data.submission_queue.sq_off_ring_entries = params.sq_off.ring_entries;
        ring_data.submission_queue.sq_off_flags = params.sq_off.flags;
        ring_data.submission_queue.sq_off_dropped = params.sq_off.dropped;
        ring_data.submission_queue.sq_off_array = params.sq_off.array;
        ring_data.submission_queue.sq_ring_ptr = pointer_value(sq.ring_ptr);
        ring_data.submission_queue.sq_ring_size = static_cast<uint64_t>(sq.ring_sz);
        ring_data.submission_queue.sq_head_ptr = pointer_value(sq.khead);
        ring_data.submission_queue.sq_tail_ptr = pointer_value(sq.ktail);
        ring_data.submission_queue.sq_flags_ptr = pointer_value(sq.kflags);
        ring_data.submission_queue.sq_dropped_ptr = pointer_value(sq.kdropped);
        ring_data.submission_queue.sq_array_ptr = pointer_value(sq.array);
        ring_data.submission_queue.sqes_ptr = pointer_value(sq.sqes);

        ring_data.completion_queue.cq_ring_mask = cq.ring_mask;
        ring_data.completion_queue.cq_ring_entries = cq.ring_entries;
        ring_data.completion_queue.cq_off_head = params.cq_off.head;
        ring_data.completion_queue.cq_off_tail = params.cq_off.tail;
        ring_data.completion_queue.cq_off_ring_mask = params.cq_off.ring_mask;
        ring_data.completion_queue.cq_off_ring_entries = params.cq_off.ring_entries;
        ring_data.completion_queue.cq_off_overflow = params.cq_off.overflow;
        ring_data.completion_queue.cq_off_cqes = params.cq_off.cqes;
        ring_data.completion_queue.cq_off_flags = params.cq_off.flags;
        ring_data.completion_queue.cq_ring_ptr = pointer_value(cq.ring_ptr);
        ring_data.completion_queue.cq_ring_size = static_cast<uint64_t>(cq.ring_sz);
        ring_data.completion_queue.cq_head_ptr = pointer_value(cq.khead);
        ring_data.completion_queue.cq_tail_ptr = pointer_value(cq.ktail);
        ring_data.completion_queue.cq_flags_ptr = pointer_value(cq.kflags);
        ring_data.completion_queue.cq_overflow_ptr = pointer_value(cq.koverflow);
        ring_data.completion_queue.cqes_ptr = pointer_value(cq.cqes);

        ring_data.buffers.buffer_region_ptr
            = pointer_value(state->buffer_region.empty() ? nullptr : state->buffer_region.data());
        ring_data.buffers.buffer_region_size = static_cast<uint64_t>(state->buffer_region.size());
        ring_data.buffers.buffer_size = state->controller_options.buffer_size;
        ring_data.buffers.buffer_count = state->controller_options.buffer_count;
        ring_data.buffers.buffers_registered = state->buffers_registered;
        ring_data.buffers.registered_buffer_count = static_cast<uint32_t>(state->registered_buffers.size());

        ring_data.fixed_files.fixed_files_registered = state->fixed_files_registered;
        ring_data.fixed_files.fixed_file_count = state->fixed_file_count;

        return rpc::error::OK();
    }

    void host_controller::close() noexcept
    {
        auto state = detach_state();
        if (!state)
        {
            return;
        }

        auto scheduled_state = state;
        if (scheduler_ && options_.cleanup_on_scheduler
            && scheduler_->spawn_detached(close_state_on_scheduler(std::move(scheduled_state))))
        {
            return;
        }

        close_state_now(state);
    }

    bool host_controller::is_open() const noexcept
    {
        auto state = get_state();
        return state && state->open.load(std::memory_order_acquire);
    }

    int host_controller::ring_fd() const noexcept
    {
        auto state = get_state();
        if (!state)
        {
            return -1;
        }

        std::lock_guard<std::mutex> lock(state->mutex);
        return state->open.load(std::memory_order_acquire) ? state->ring.ring_fd : -1;
    }

    std::shared_ptr<host_controller::state> host_controller::get_state() const noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_;
    }

    std::shared_ptr<host_controller::state> host_controller::detach_state() noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto state = std::move(state_);
        state_.reset();
        return state;
    }

    void host_controller::close_state_now(const std::shared_ptr<state>& state) noexcept
    {
        if (!state)
        {
            return;
        }

        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->open.exchange(false, std::memory_order_acq_rel))
        {
            return;
        }

        io_uring_queue_exit(&state->ring);
        state->fixed_files_registered = false;
        state->fixed_file_count = 0;
        state->buffers_registered = false;
        state->registered_buffers.clear();
        state->buffer_region.clear();
    }

    CORO_TASK(void) host_controller::close_state_on_scheduler(std::shared_ptr<state> state)
    {
        close_state_now(state);
        CO_RETURN;
    }

} // namespace rpc::io_uring
