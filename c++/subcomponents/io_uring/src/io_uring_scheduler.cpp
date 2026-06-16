/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <io_uring/io_uring_scheduler.h>

#include <exception>
#include <new>
#include <utility>

namespace rpc::io_uring
{
    std::shared_ptr<io_uring_scheduler> io_uring_scheduler::create(std::shared_ptr<rpc::coro::scheduler> scheduler)
    {
        try
        {
            if (!scheduler)
                scheduler = rpc::coro::make_shared_scheduler();
            return std::make_shared<io_uring_scheduler>(std::move(scheduler));
        }
        catch (const std::bad_alloc&)
        {
            RPC_ERROR("bad_alloc while creating io_uring scheduler");
            std::terminate();
        }
        return {};
    }

    io_uring_scheduler::io_uring_scheduler(std::shared_ptr<rpc::coro::scheduler> scheduler) noexcept
        : scheduler_(std::move(scheduler))
    {
    }

    io_uring_scheduler::~io_uring_scheduler()
    {
        shutdown();
    }

    std::shared_ptr<rpc::coro::scheduler> io_uring_scheduler::scheduler() const noexcept
    {
        return scheduler_;
    }

    rpc::coro::scheduler* io_uring_scheduler::scheduler_ptr() const noexcept
    {
        return scheduler_.get();
    }

    std::shared_ptr<controller> io_uring_scheduler::get_controller() const noexcept
    {
        return controller_;
    }

    void io_uring_scheduler::set_controller(std::shared_ptr<controller> controller) noexcept
    {
        controller_ = std::move(controller);
    }

    void io_uring_scheduler::request_controller_shutdown() noexcept
    {
        if (controller_)
            controller_->request_shutdown();
    }

    void io_uring_scheduler::shutdown() noexcept
    {
        request_controller_shutdown();
        controller_.reset();
        if (scheduler_)
            scheduler_->shutdown();
        scheduler_.reset();
    }
} // namespace rpc::io_uring
