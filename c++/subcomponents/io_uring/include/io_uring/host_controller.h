/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>

#include <io_uring/io_uring.h>
#include <rpc/rpc.h>

namespace coro
{
    class scheduler;
}

namespace rpc::io_uring
{
    struct host_controller_options
    {
        uint32_t queue_depth{256};
#ifdef CANOPY_IO_URING_SQPOLL
        // Enclave users submit SQEs directly and cannot make io_uring_enter()
        // themselves. SQPOLL gives the host a kernel-side submitter thread that
        // can be woken through i_host_io_uring_control::wake_iouring().
        bool use_sqpoll{true};
#else
        // Non-SQPOLL builds are useful for host-only experiments, but the SGX
        // no-op smoke path expects SQPOLL because the enclave cannot enter the
        // ring directly.
        bool use_sqpoll{false};
#endif
        // SINGLE_ISSUER is only requested with SQPOLL. The host controller owns
        // one ring and the intended model is a single enclave agent writing
        // SQEs for that ring; sharing one ring between enclaves would need a
        // different synchronization design.
        bool use_single_issuer{true};
        // COOP_TASKRUN is deliberately off for the SGX/SQPOLL path. It is a
        // userspace task-run hint for non-SQPOLL rings, while this controller
        // relies on the SQPOLL thread plus explicit wake_iouring().
        bool use_coop_taskrun{false};
        // Kernel SQPOLL idle timeout before the polling thread sleeps. A later
        // wake_iouring() uses IORING_ENTER_SQ_WAKEUP to reactivate it.
        uint32_t sq_thread_idle_ms{2000};
        uint32_t buffer_count{256};
        uint32_t buffer_size{4096};
        bool register_buffers{false};
        // Direct descriptors are private to the io_uring instance. Enclave TCP
        // operations use them so SQEs never need to expose ordinary host file
        // descriptor numbers or require enclave-side fd ownership.
        uint32_t fixed_file_count{0};
        bool register_fixed_files{false};
        bool cleanup_on_scheduler{true};
    };

    class host_controller
    {
    public:
        using options = host_controller_options;

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
        std::shared_ptr<coro::scheduler> scheduler_;
        std::shared_ptr<state> state_;
        mutable std::mutex mutex_;
    };

} // namespace rpc::io_uring
