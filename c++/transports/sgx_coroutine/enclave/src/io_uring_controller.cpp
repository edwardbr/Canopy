/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <transports/sgx_coroutine/enclave/io_uring_controller.h>
#include <transports/sgx_coroutine/enclave/host_transport.h>

#include <utility>

namespace rpc::sgx::coro::enclave
{
    enclave_io_uring_handle::enclave_io_uring_handle(std::weak_ptr<host_transport> host_transport)
        : host_transport_(std::move(host_transport))
    {
    }

    CORO_TASK(int)
    enclave_io_uring_handle::get_iouring_data(rpc::io_uring::data& ring_data)
    {
        auto transport = host_transport_.lock();
        if (!transport)
            CO_RETURN rpc::error::TRANSPORT_ERROR();

        CO_RETURN CO_AWAIT transport->get_iouring_data(ring_data);
    }

    CORO_TASK(int)
    enclave_io_uring_handle::notify_submitted(
        const rpc::io_uring::data&,
        uint32_t)
    {
        auto transport = host_transport_.lock();
        if (!transport)
            CO_RETURN rpc::error::TRANSPORT_ERROR();

        CO_RETURN CO_AWAIT transport->wake_host_iouring();
    }

    void enclave_io_uring_handle::close() noexcept
    {
        host_transport_.reset();
    }

    enclave_io_uring_controller::enclave_io_uring_controller(
        rpc::coro::scheduler* scheduler,
        std::weak_ptr<host_transport> host_transport)
        : rpc::io_uring::controller(
              std::make_shared<enclave_io_uring_handle>(std::move(host_transport)),
              scheduler)
    {
    }

    enclave_io_uring_controller::~enclave_io_uring_controller()
    {
        request_shutdown();
    }
}
