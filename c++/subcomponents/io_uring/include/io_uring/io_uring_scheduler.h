/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <memory>

#include <io_uring/controller.h>

namespace rpc::io_uring
{
    class io_uring_scheduler
    {
    public:
        [[nodiscard]] static std::shared_ptr<io_uring_scheduler> create(
            std::shared_ptr<rpc::coro::scheduler> scheduler = {});

        explicit io_uring_scheduler(std::shared_ptr<rpc::coro::scheduler> scheduler) noexcept;
        ~io_uring_scheduler();

        io_uring_scheduler(const io_uring_scheduler&) = delete;
        io_uring_scheduler& operator=(const io_uring_scheduler&) = delete;
        io_uring_scheduler(io_uring_scheduler&&) = delete;
        io_uring_scheduler& operator=(io_uring_scheduler&&) = delete;

        [[nodiscard]] std::shared_ptr<rpc::coro::scheduler> scheduler() const noexcept;
        [[nodiscard]] rpc::coro::scheduler* scheduler_ptr() const noexcept;

        [[nodiscard]] std::shared_ptr<controller> get_controller() const noexcept;
        void set_controller(std::shared_ptr<controller> controller) noexcept;
        void request_controller_shutdown() noexcept;

        // Shuts the controller down before the scheduler. Runtime teardown owns
        // this order so services can still use the unchanged scheduler pointer.
        void shutdown() noexcept;

    private:
        std::shared_ptr<rpc::coro::scheduler> scheduler_;
        std::shared_ptr<controller> controller_;
    };
} // namespace rpc::io_uring
