/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <memory>
#include <mutex>

#include <io_uring/host_controller.h>
#include <io_uring/io_uring_handle.h>

namespace rpc::io_uring
{
    class linux_io_uring_handle : public io_uring_handle
    {
    public:
        using options = host_controller::options;

        [[nodiscard]] static int create(
            std::shared_ptr<linux_io_uring_handle>& handle,
            options handle_options = {},
            std::shared_ptr<coro::scheduler> scheduler = {}) noexcept;

        explicit linux_io_uring_handle(std::unique_ptr<host_controller> controller) noexcept;
        ~linux_io_uring_handle() override;

        linux_io_uring_handle(const linux_io_uring_handle&) = delete;
        linux_io_uring_handle& operator=(const linux_io_uring_handle&) = delete;
        linux_io_uring_handle(linux_io_uring_handle&&) = delete;
        linux_io_uring_handle& operator=(linux_io_uring_handle&&) = delete;

        CORO_TASK(int) get_iouring_data(data& ring_data) override;
        CORO_TASK(int)
        notify_submitted(
            const data& ring_data,
            uint32_t sqe_count) override;
        void close() noexcept override;

        [[nodiscard]] bool is_open() const noexcept;
        [[nodiscard]] int ring_fd() const noexcept;

    private:
        [[nodiscard]] host_controller* controller_locked() const noexcept;

        mutable std::mutex mutex_;
        std::unique_ptr<host_controller> controller_;
    };
} // namespace rpc::io_uring
