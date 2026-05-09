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

    class enclave_io_uring_controller : public rpc::io_uring::controller
    {
    public:
        enclave_io_uring_controller(
            rpc::coro::scheduler* scheduler,
            std::weak_ptr<host_transport> host_transport);

        virtual ~enclave_io_uring_controller();

    private:
        CORO_TASK(int) inner_wake_host_iouring() override;
        CORO_TASK(int) inner_get_iouring_data(rpc::io_uring::data& ring_data) override;

        std::weak_ptr<host_transport> host_transport_;
    };
}
