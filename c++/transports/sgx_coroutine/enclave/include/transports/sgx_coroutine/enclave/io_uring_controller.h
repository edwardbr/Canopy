/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <array>
#include <chrono>
#include <edl/coroutine_enclave.h>
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
        CORO_TASK(rpc::io_uring::descriptor_result) create_tcp_ipv4_socket() override;
        CORO_TASK(rpc::io_uring::descriptor_result) create_tcp_ipv6_socket() override;
        CORO_TASK(rpc::io_uring::operation_result) set_socket_reuse_addr(uint32_t descriptor) override;
        CORO_TASK(rpc::io_uring::operation_result)
        bind_tcp_ipv4_loopback(
            uint32_t descriptor,
            uint16_t port) override;
        CORO_TASK(rpc::io_uring::operation_result)
        bind_tcp_ipv4(
            uint32_t descriptor,
            const std::array<
                uint8_t,
                4>& address,
            uint16_t port) override;
        CORO_TASK(rpc::io_uring::operation_result)
        bind_tcp_ipv6(
            uint32_t descriptor,
            const std::array<
                uint8_t,
                16>& address,
            uint16_t port) override;
        CORO_TASK(rpc::io_uring::operation_result)
        listen(
            uint32_t descriptor,
            uint32_t backlog) override;
        CORO_TASK(rpc::io_uring::descriptor_result) accept(uint32_t descriptor) override;
        CORO_TASK(rpc::io_uring::descriptor_result) connect_tcp_ipv4_loopback(uint16_t port) override;
        CORO_TASK(rpc::io_uring::operation_result) set_tcp_no_delay(uint32_t descriptor) override;
        CORO_TASK(rpc::io_uring::transfer_result)
        send(
            uint32_t descriptor,
            rpc::byte_span buffer,
            uint32_t msg_flags) override;
        CORO_TASK(rpc::io_uring::transfer_result)
        receive(
            uint32_t descriptor,
            rpc::mutable_byte_span buffer,
            uint32_t msg_flags) override;
        CORO_TASK(rpc::io_uring::operation_result) cancel_direct(uint32_t descriptor) override;
        CORO_TASK(rpc::io_uring::operation_result) close_direct(uint32_t descriptor) override;
        void close() noexcept override;

    private:
        CORO_TASK(int)
        host_tcp_operation(
            rpc::sgx::coro::protocol::host_tcp_request request,
            rpc::sgx::coro::protocol::host_tcp_result& result);

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
