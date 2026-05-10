/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <io_uring/linux_io_uring_handle.h>

#include <exception>
#include <new>
#include <utility>

namespace rpc::io_uring
{
    int linux_io_uring_handle::create(
        std::shared_ptr<linux_io_uring_handle>& handle,
        options handle_options,
        std::shared_ptr<coro::scheduler> scheduler) noexcept
    {
        handle.reset();

        std::unique_ptr<host_controller> controller;
        auto err = host_controller::create(controller, handle_options, std::move(scheduler));
        if (err != rpc::error::OK())
        {
            return err;
        }

        try
        {
            handle = std::make_shared<linux_io_uring_handle>(std::move(controller));
        }
        catch (const std::bad_alloc&)
        {
            RPC_ERROR("bad_alloc while creating linux_io_uring_handle");
            std::terminate();
        }

        return rpc::error::OK();
    }

    linux_io_uring_handle::linux_io_uring_handle(std::unique_ptr<host_controller> controller) noexcept
        : controller_(std::move(controller))
    {
    }

    linux_io_uring_handle::~linux_io_uring_handle()
    {
        close();
    }

    CORO_TASK(int) linux_io_uring_handle::get_iouring_data(data& ring_data)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* controller = controller_locked();
        if (!controller)
            CO_RETURN rpc::error::RESOURCE_CLOSED();

        CO_RETURN controller->get_iouring_data(ring_data);
    }

    CORO_TASK(int)
    linux_io_uring_handle::notify_submitted(
        const data&,
        uint32_t)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* controller = controller_locked();
        if (!controller)
            CO_RETURN rpc::error::RESOURCE_CLOSED();

        // The first host-direct slice still uses the same SQPOLL direct-ring
        // path as enclaves. Non-SQPOLL host submission can be added here without
        // changing the common operation controller.
        CO_RETURN controller->wake_iouring();
    }

    void linux_io_uring_handle::close() noexcept
    {
        std::unique_ptr<host_controller> controller;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            controller = std::move(controller_);
        }

        if (controller)
            controller->close();
    }

    bool linux_io_uring_handle::is_open() const noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* controller = controller_locked();
        return controller && controller->is_open();
    }

    int linux_io_uring_handle::ring_fd() const noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* controller = controller_locked();
        return controller ? controller->ring_fd() : -1;
    }

    host_controller* linux_io_uring_handle::controller_locked() const noexcept
    {
        return controller_.get();
    }
} // namespace rpc::io_uring
