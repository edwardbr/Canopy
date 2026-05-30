/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <memory>

#include <io_uring/host_controller.h>
#include <io_uring/io_uring_scheduler.h>
#include <io_uring/linux_io_uring_handle.h>

namespace rpc::io_uring
{
    [[nodiscard]] controller::options default_controller_options() noexcept;
    [[nodiscard]] host_controller::options default_host_controller_options() noexcept;
    [[nodiscard]] host_controller::options default_enclave_host_controller_options() noexcept;
    [[nodiscard]] host_controller::options host_controller_options_from_enclave_host_options(
        const ::canopy::io_uring::enclave_host_controller_options& options) noexcept;

    [[nodiscard]] int create_scheduler(
        std::shared_ptr<io_uring_scheduler>& scheduler_owner,
        linux_io_uring_handle::options handle_options = {},
        std::shared_ptr<rpc::coro::scheduler> scheduler = {},
        controller::options controller_options = default_controller_options()) noexcept;
} // namespace rpc::io_uring
