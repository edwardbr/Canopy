/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>

#include <rpc/rpc.h>

namespace rpc::io_uring
{
    class controller;

    class direct_descriptor
    {
    public:
        static constexpr uint32_t invalid_descriptor = std::numeric_limits<uint32_t>::max();

        // Owns one kernel fixed-file-table slot allocated by the host io_uring.
        // The value is not a process fd inside the enclave; it must be used with
        // SQEs carrying IOSQE_FIXED_FILE/direct-descriptor semantics.
        direct_descriptor(
            std::shared_ptr<controller> owner,
            uint32_t descriptor) noexcept;
        ~direct_descriptor();

        direct_descriptor(const direct_descriptor&) = delete;
        auto operator=(const direct_descriptor&) -> direct_descriptor& = delete;

        direct_descriptor(direct_descriptor&& other) noexcept;
        auto operator=(direct_descriptor&& other) noexcept -> direct_descriptor&;

        [[nodiscard]] bool is_open() const noexcept;
        [[nodiscard]] uint32_t get() const noexcept;
        [[nodiscard]] std::shared_ptr<controller> get_controller() const noexcept;

        // Deterministically closes the direct descriptor. Destruction also
        // attempts cleanup, but callers should prefer awaiting close when the
        // coroutine scope can report errors.
        CORO_TASK(int) close();

    private:
        static void close_detached(
            std::shared_ptr<controller> owner,
            uint32_t descriptor) noexcept;

        std::shared_ptr<controller> controller_;
        std::atomic<uint32_t> descriptor_{invalid_descriptor};
    };
} // namespace rpc::io_uring
