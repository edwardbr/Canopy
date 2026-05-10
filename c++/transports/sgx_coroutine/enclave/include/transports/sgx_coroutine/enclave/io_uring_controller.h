/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <io_uring/controller.h>
#include <memory>

namespace rpc::sgx::coro::enclave
{
    class host_transport;

    class enclave_io_uring_handle : public rpc::io_uring::io_uring_handle
    {
    public:
        explicit enclave_io_uring_handle(std::weak_ptr<host_transport> host_transport);
        ~enclave_io_uring_handle() override = default;

        CORO_TASK(int) get_iouring_data(rpc::io_uring::data& ring_data) override;
        CORO_TASK(int)
        notify_submitted(
            const rpc::io_uring::data& ring_data,
            uint32_t sqe_count) override;
        void close() noexcept override;

    private:
        std::weak_ptr<host_transport> host_transport_;
    };

    // Transitional compatibility wrapper. The common controller now depends on
    // io_uring_handle; this class keeps existing enclave runtime construction
    // stable while the naming is migrated.
    class enclave_io_uring_controller : public rpc::io_uring::controller
    {
    public:
        enclave_io_uring_controller(
            rpc::coro::scheduler* scheduler,
            std::weak_ptr<host_transport> host_transport);

        virtual ~enclave_io_uring_controller();
    };
}
