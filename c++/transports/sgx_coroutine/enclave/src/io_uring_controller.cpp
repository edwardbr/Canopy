/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <transports/sgx_coroutine/enclave/io_uring_controller.h>
#include <transports/sgx_coroutine/enclave/host_transport.h>

#include <utility>

namespace rpc::sgx::coro::enclave
{
    enclave_io_uring_controller::enclave_io_uring_controller(
        rpc::coro::scheduler* scheduler,
        std::weak_ptr<host_transport> host_transport)
        : rpc::io_uring::controller(scheduler)
        , host_transport_(std::move(host_transport))
    {
    }

    enclave_io_uring_controller::~enclave_io_uring_controller()
    {
        request_shutdown();
    }

    CORO_TASK(int)
    enclave_io_uring_controller::inner_wake_host_iouring()
    {
        auto transport = host_transport_.lock();
        if (!transport)
            CO_RETURN rpc::error::TRANSPORT_ERROR();

        CO_RETURN CO_AWAIT transport->wake_host_iouring();
    }

    CORO_TASK(int)
    enclave_io_uring_controller::inner_get_iouring_data(rpc::io_uring::data& ring_data)
    {
        auto transport = host_transport_.lock();
        if (!transport)
            CO_RETURN rpc::error::TRANSPORT_ERROR();

        CO_RETURN CO_AWAIT transport->get_iouring_data(ring_data);
    }
}
