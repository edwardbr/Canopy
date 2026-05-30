/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <io_uring/host_io_uring.h>

#include <exception>
#include <new>
#include <utility>

namespace rpc::io_uring
{
    controller::options default_controller_options() noexcept
    {
        controller::options options;
        options.completion_wait_strategy = wait_strategy::proactor;
        // Non-enclave streams can safely submit their per-call byte spans
        // directly to the kernel. SGX paths must override this to false so
        // transfers stage through staging buffers.
        options.use_caller_buffers_for_transfers = true;
        return options;
    }

    host_controller::options default_host_controller_options() noexcept
    {
        return {};
    }

    host_controller::options host_controller_options_from_enclave_host_options(
        const ::canopy::io_uring::enclave_host_controller_options& options) noexcept
    {
        host_controller::options result;
        result.queue_depth = options.queue_depth;
        result.use_sqpoll = options.use_sqpoll;
        result.use_single_issuer = options.use_single_issuer;
        result.use_coop_taskrun = options.use_coop_taskrun;
        result.sq_thread_idle_ms = options.sq_thread_idle_ms;
        result.buffer_count = options.buffer_count;
        result.buffer_size = options.buffer_size;
        result.register_buffers = options.register_buffers;
        result.fixed_file_count = options.fixed_file_count;
        result.register_fixed_files = options.register_fixed_files;
        result.cleanup_on_scheduler = options.cleanup_on_scheduler;
        return result;
    }

    host_controller::options default_enclave_host_controller_options() noexcept
    {
        return host_controller_options_from_enclave_host_options(::canopy::io_uring::enclave_host_controller_options{});
    }

    int create_scheduler(
        std::shared_ptr<io_uring_scheduler>& scheduler_owner,
        linux_io_uring_handle::options handle_options,
        std::shared_ptr<rpc::coro::scheduler> scheduler,
        controller::options controller_options) noexcept
    {
        scheduler_owner.reset();

        auto owner = io_uring_scheduler::create(std::move(scheduler));

        std::shared_ptr<linux_io_uring_handle> handle;
        auto err = linux_io_uring_handle::create(handle, handle_options, owner->scheduler());
        if (err != rpc::error::OK())
        {
            owner->shutdown();
            return err;
        }

        std::shared_ptr<controller> ring_controller;
        try
        {
            ring_controller = std::make_shared<controller>(std::move(handle), owner->scheduler_ptr(), controller_options);
        }
        catch (const std::bad_alloc&)
        {
            RPC_ERROR("bad_alloc while creating io_uring controller");
            std::terminate();
        }

        owner->set_controller(std::move(ring_controller));
        scheduler_owner = std::move(owner);
        return rpc::error::OK();
    }
} // namespace rpc::io_uring
