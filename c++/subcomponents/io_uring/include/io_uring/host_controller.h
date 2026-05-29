/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>

#include <io_uring/io_uring_config.h>
#include <io_uring/types.h>
#include <rpc/rpc.h>

namespace coro
{
    class scheduler;
}

namespace rpc::io_uring
{
    class host_controller
    {
    public:
        using options = ::canopy::io_uring::host_controller_options;

        [[nodiscard]] static int create(
            std::unique_ptr<host_controller>& controller,
            options controller_options = {},
            std::shared_ptr<coro::scheduler> scheduler = {}) noexcept;
        ~host_controller();

        host_controller(const host_controller&) = delete;
        host_controller& operator=(const host_controller&) = delete;
        host_controller(host_controller&&) = delete;
        host_controller& operator=(host_controller&&) = delete;

        [[nodiscard]] int wake_iouring() noexcept;
        [[nodiscard]] int get_iouring_data(data& ring_data) noexcept;
        void close() noexcept;

        [[nodiscard]] bool is_open() const noexcept;
        [[nodiscard]] int ring_fd() const noexcept;
        [[nodiscard]] const options& get_options() const noexcept { return options_; }

    private:
        struct state;

        host_controller(
            options controller_options,
            std::shared_ptr<coro::scheduler> scheduler,
            std::shared_ptr<state> state) noexcept;

        [[nodiscard]] std::shared_ptr<state> get_state() const noexcept;
        [[nodiscard]] std::shared_ptr<state> detach_state() noexcept;

        static void close_state_now(const std::shared_ptr<state>& state) noexcept;
        static CORO_TASK(void) close_state_on_scheduler(std::shared_ptr<state> state);

        options options_;
        // The controller may outlive the scheduler during transport teardown.
        // Keep this weak so delayed descriptor cleanup cannot extend the
        // scheduler lifetime and make benchmark/test shutdown look hung.
        std::weak_ptr<coro::scheduler> scheduler_;
        std::shared_ptr<state> state_;
        mutable std::mutex mutex_;
    };

} // namespace rpc::io_uring
