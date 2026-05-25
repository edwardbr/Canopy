/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <transports/sgx_coroutine/enclave/io_uring_controller.h>
#include <transports/sgx_coroutine/enclave/host_transport.h>

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

namespace rpc::sgx::coro::enclave
{
    namespace
    {
        namespace protocol = rpc::sgx::coro::protocol;

        [[nodiscard]] auto to_descriptor_result(const protocol::host_tcp_result& result) -> rpc::io_uring::descriptor_result
        {
            return rpc::io_uring::descriptor_result{result.error_code,
                result.descriptor < 0 ? 0U : static_cast<uint32_t>(result.descriptor),
                result.native_result,
                result.cqe_flags};
        }

        [[nodiscard]] auto to_operation_result(const protocol::host_tcp_result& result) -> rpc::io_uring::operation_result
        {
            return rpc::io_uring::operation_result{result.error_code, result.native_result, result.cqe_flags};
        }

        [[nodiscard]] auto to_transfer_result(const protocol::host_tcp_result& result) -> rpc::io_uring::transfer_result
        {
            return rpc::io_uring::transfer_result{
                result.error_code, result.bytes_transferred, result.native_result, result.cqe_flags};
        }

        template<size_t Size>
        [[nodiscard]] auto to_vector(
            const std::array<
                uint8_t,
                Size>& address) -> std::vector<uint8_t>
        {
            return std::vector<uint8_t>(address.begin(), address.end());
        }
    } // namespace

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

    CORO_TASK(int)
    enclave_io_uring_handle::host_tcp_operation(
        protocol::host_tcp_request request,
        protocol::host_tcp_result& result)
    {
        auto transport = host_transport_.lock();
        if (!transport)
            CO_RETURN rpc::error::TRANSPORT_ERROR();

        CO_RETURN CO_AWAIT transport->host_tcp_operation(std::move(request), result);
    }

    CORO_TASK(rpc::io_uring::descriptor_result)
    enclave_io_uring_handle::create_tcp_ipv4_socket()
    {
        protocol::host_tcp_result result;
        protocol::host_tcp_request request;
        request.operation = protocol::host_tcp_operation::tcp_create_ipv4_socket;
        auto err = CO_AWAIT host_tcp_operation(std::move(request), result);
        if (err != rpc::error::OK())
            CO_RETURN rpc::io_uring::descriptor_result{err, 0, 0, 0};
        CO_RETURN to_descriptor_result(result);
    }

    CORO_TASK(rpc::io_uring::descriptor_result)
    enclave_io_uring_handle::create_tcp_ipv6_socket()
    {
        protocol::host_tcp_result result;
        protocol::host_tcp_request request;
        request.operation = protocol::host_tcp_operation::tcp_create_ipv6_socket;
        auto err = CO_AWAIT host_tcp_operation(std::move(request), result);
        if (err != rpc::error::OK())
            CO_RETURN rpc::io_uring::descriptor_result{err, 0, 0, 0};
        CO_RETURN to_descriptor_result(result);
    }

    CORO_TASK(rpc::io_uring::operation_result)
    enclave_io_uring_handle::set_socket_reuse_addr(uint32_t descriptor)
    {
        protocol::host_tcp_result result;
        protocol::host_tcp_request request;
        request.operation = protocol::host_tcp_operation::tcp_set_socket_reuse_addr;
        request.descriptor = descriptor;
        auto err = CO_AWAIT host_tcp_operation(std::move(request), result);
        if (err != rpc::error::OK())
            CO_RETURN rpc::io_uring::operation_result{err, 0, 0};
        CO_RETURN to_operation_result(result);
    }

    CORO_TASK(rpc::io_uring::operation_result)
    enclave_io_uring_handle::bind_tcp_ipv4_loopback(
        uint32_t descriptor,
        uint16_t port)
    {
        protocol::host_tcp_result result;
        protocol::host_tcp_request request;
        request.operation = protocol::host_tcp_operation::tcp_bind_ipv4_loopback;
        request.descriptor = descriptor;
        request.value = port;
        auto err = CO_AWAIT host_tcp_operation(std::move(request), result);
        if (err != rpc::error::OK())
            CO_RETURN rpc::io_uring::operation_result{err, 0, 0};
        CO_RETURN to_operation_result(result);
    }

    CORO_TASK(rpc::io_uring::operation_result)
    enclave_io_uring_handle::bind_tcp_ipv4(
        uint32_t descriptor,
        const std::array<
            uint8_t,
            4>& address,
        uint16_t port)
    {
        protocol::host_tcp_result result;
        protocol::host_tcp_request request;
        request.operation = protocol::host_tcp_operation::tcp_bind_ipv4;
        request.descriptor = descriptor;
        request.value = port;
        request.address = to_vector(address);
        auto err = CO_AWAIT host_tcp_operation(std::move(request), result);
        if (err != rpc::error::OK())
            CO_RETURN rpc::io_uring::operation_result{err, 0, 0};
        CO_RETURN to_operation_result(result);
    }

    CORO_TASK(rpc::io_uring::operation_result)
    enclave_io_uring_handle::bind_tcp_ipv6(
        uint32_t descriptor,
        const std::array<
            uint8_t,
            16>& address,
        uint16_t port)
    {
        protocol::host_tcp_result result;
        protocol::host_tcp_request request;
        request.operation = protocol::host_tcp_operation::tcp_bind_ipv6;
        request.descriptor = descriptor;
        request.value = port;
        request.address = to_vector(address);
        auto err = CO_AWAIT host_tcp_operation(std::move(request), result);
        if (err != rpc::error::OK())
            CO_RETURN rpc::io_uring::operation_result{err, 0, 0};
        CO_RETURN to_operation_result(result);
    }

    CORO_TASK(rpc::io_uring::operation_result)
    enclave_io_uring_handle::listen(
        uint32_t descriptor,
        uint32_t backlog)
    {
        protocol::host_tcp_result result;
        protocol::host_tcp_request request;
        request.operation = protocol::host_tcp_operation::tcp_listen;
        request.descriptor = descriptor;
        request.value = backlog;
        auto err = CO_AWAIT host_tcp_operation(std::move(request), result);
        if (err != rpc::error::OK())
            CO_RETURN rpc::io_uring::operation_result{err, 0, 0};
        CO_RETURN to_operation_result(result);
    }

    CORO_TASK(rpc::io_uring::descriptor_result)
    enclave_io_uring_handle::accept(uint32_t descriptor)
    {
        protocol::host_tcp_result result;
        protocol::host_tcp_request request;
        request.operation = protocol::host_tcp_operation::tcp_accept;
        request.descriptor = descriptor;
        auto err = CO_AWAIT host_tcp_operation(std::move(request), result);
        if (err != rpc::error::OK())
            CO_RETURN rpc::io_uring::descriptor_result{err, 0, 0, 0};
        CO_RETURN to_descriptor_result(result);
    }

    CORO_TASK(rpc::io_uring::descriptor_result)
    enclave_io_uring_handle::connect_tcp_ipv4_loopback(uint16_t port)
    {
        protocol::host_tcp_result result;
        protocol::host_tcp_request request;
        request.operation = protocol::host_tcp_operation::tcp_connect_ipv4_loopback;
        request.value = port;
        auto err = CO_AWAIT host_tcp_operation(std::move(request), result);
        if (err != rpc::error::OK())
            CO_RETURN rpc::io_uring::descriptor_result{err, 0, 0, 0};
        CO_RETURN to_descriptor_result(result);
    }

    CORO_TASK(rpc::io_uring::operation_result)
    enclave_io_uring_handle::set_tcp_no_delay(uint32_t descriptor)
    {
        protocol::host_tcp_result result;
        protocol::host_tcp_request request;
        request.operation = protocol::host_tcp_operation::tcp_set_tcp_no_delay;
        request.descriptor = descriptor;
        auto err = CO_AWAIT host_tcp_operation(std::move(request), result);
        if (err != rpc::error::OK())
            CO_RETURN rpc::io_uring::operation_result{err, 0, 0};
        CO_RETURN to_operation_result(result);
    }

    CORO_TASK(rpc::io_uring::transfer_result)
    enclave_io_uring_handle::send(
        uint32_t descriptor,
        rpc::byte_span buffer,
        uint32_t msg_flags)
    {
        protocol::host_tcp_result result;
        protocol::host_tcp_request request;
        request.operation = protocol::host_tcp_operation::tcp_send;
        request.descriptor = descriptor;
        request.flags = msg_flags;
        request.payload.assign(buffer.begin(), buffer.end());
        auto err = CO_AWAIT host_tcp_operation(std::move(request), result);
        if (err != rpc::error::OK())
            CO_RETURN rpc::io_uring::transfer_result{err, 0, 0, 0};
        CO_RETURN to_transfer_result(result);
    }

    CORO_TASK(rpc::io_uring::transfer_result)
    enclave_io_uring_handle::receive(
        uint32_t descriptor,
        rpc::mutable_byte_span buffer,
        uint32_t msg_flags)
    {
        protocol::host_tcp_result result;
        protocol::host_tcp_request request;
        request.operation = (msg_flags & 0x40U) != 0 ? protocol::host_tcp_operation::tcp_receive_nonblocking
                                                     : protocol::host_tcp_operation::tcp_receive;
        request.descriptor = descriptor;
        request.value = static_cast<uint32_t>(
            std::min<size_t>(buffer.size(), static_cast<size_t>(std::numeric_limits<uint32_t>::max())));
        request.flags = msg_flags;
        auto err = CO_AWAIT host_tcp_operation(std::move(request), result);
        if (err != rpc::error::OK())
            CO_RETURN rpc::io_uring::transfer_result{err, 0, 0, 0};
        if (result.error_code == rpc::error::OK() && !result.payload.empty())
        {
            const auto bytes = std::min(buffer.size(), result.payload.size());
            std::copy_n(result.payload.data(), bytes, buffer.data());
            result.bytes_transferred = static_cast<uint32_t>(bytes);
        }
        CO_RETURN to_transfer_result(result);
    }

    CORO_TASK(rpc::io_uring::operation_result)
    enclave_io_uring_handle::cancel_direct(uint32_t descriptor)
    {
        protocol::host_tcp_result result;
        protocol::host_tcp_request request;
        request.operation = protocol::host_tcp_operation::tcp_cancel;
        request.descriptor = descriptor;
        auto err = CO_AWAIT host_tcp_operation(std::move(request), result);
        if (err != rpc::error::OK())
            CO_RETURN rpc::io_uring::operation_result{err, 0, 0};
        CO_RETURN to_operation_result(result);
    }

    CORO_TASK(rpc::io_uring::operation_result)
    enclave_io_uring_handle::close_direct(uint32_t descriptor)
    {
        protocol::host_tcp_result result;
        protocol::host_tcp_request request;
        request.operation = protocol::host_tcp_operation::tcp_close;
        request.descriptor = descriptor;
        auto err = CO_AWAIT host_tcp_operation(std::move(request), result);
        if (err != rpc::error::OK())
            CO_RETURN rpc::io_uring::operation_result{err, 0, 0};
        CO_RETURN to_operation_result(result);
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
