/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include <io_uring/types.h>
#include <rpc/rpc.h>

namespace rpc::io_uring
{
    class io_uring_handle
    {
    public:
        virtual ~io_uring_handle() = default;

        virtual CORO_TASK(int) get_iouring_data(data& ring_data) = 0;

        // Called after the controller has published one or more SQEs. The
        // handle decides whether that means an SQPOLL wake, an io_uring_enter,
        // or no action for the current environment.
        virtual CORO_TASK(int) notify_submitted(
            data ring_data,
            uint32_t sqe_count) = 0;

        virtual CORO_TASK(descriptor_result) open_file(
            std::string,
            uint32_t,
            uint32_t)
        {
            CO_RETURN descriptor_result{rpc::error::PROTOCOL_ERROR(), 0, 0, 0};
        }

        virtual CORO_TASK(transfer_result) read_at(
            uint32_t,
            rpc::mutable_byte_span,
            uint64_t)
        {
            CO_RETURN transfer_result{rpc::error::PROTOCOL_ERROR(), 0, 0, 0};
        }

        virtual CORO_TASK(transfer_result) write_at(
            uint32_t,
            rpc::byte_span,
            uint64_t)
        {
            CO_RETURN transfer_result{rpc::error::PROTOCOL_ERROR(), 0, 0, 0};
        }

        virtual CORO_TASK(descriptor_result) create_tcp_ipv4_socket()
        {
            CO_RETURN descriptor_result{rpc::error::PROTOCOL_ERROR(), 0, 0, 0};
        }

        virtual CORO_TASK(descriptor_result) create_tcp_ipv6_socket()
        {
            CO_RETURN descriptor_result{rpc::error::PROTOCOL_ERROR(), 0, 0, 0};
        }

        virtual CORO_TASK(operation_result) set_socket_reuse_addr(uint32_t)
        {
            CO_RETURN operation_result{rpc::error::PROTOCOL_ERROR(), 0, 0};
        }

        virtual CORO_TASK(operation_result) bind_tcp_ipv4_loopback(
            uint32_t,
            uint16_t)
        {
            CO_RETURN operation_result{rpc::error::PROTOCOL_ERROR(), 0, 0};
        }

        virtual CORO_TASK(operation_result) bind_tcp_ipv4(
            uint32_t,
            std::array<
                uint8_t,
                4>,
            uint16_t)
        {
            CO_RETURN operation_result{rpc::error::PROTOCOL_ERROR(), 0, 0};
        }

        virtual CORO_TASK(operation_result) bind_tcp_ipv6(
            uint32_t,
            std::array<
                uint8_t,
                16>,
            uint16_t)
        {
            CO_RETURN operation_result{rpc::error::PROTOCOL_ERROR(), 0, 0};
        }

        virtual CORO_TASK(operation_result) listen(
            uint32_t,
            uint32_t)
        {
            CO_RETURN operation_result{rpc::error::PROTOCOL_ERROR(), 0, 0};
        }

        virtual CORO_TASK(descriptor_result) accept(uint32_t)
        {
            CO_RETURN descriptor_result{rpc::error::PROTOCOL_ERROR(), 0, 0, 0};
        }

        virtual CORO_TASK(descriptor_result) connect_tcp_ipv4_loopback(
            uint16_t,
            std::chrono::milliseconds)
        {
            CO_RETURN descriptor_result{rpc::error::PROTOCOL_ERROR(), 0, 0, 0};
        }

        virtual CORO_TASK(descriptor_result) connect_tcp_ipv4(
            std::array<
                uint8_t,
                4>,
            uint16_t,
            std::chrono::milliseconds)
        {
            CO_RETURN descriptor_result{rpc::error::PROTOCOL_ERROR(), 0, 0, 0};
        }

        virtual CORO_TASK(descriptor_result) connect_tcp_ipv6(
            std::array<
                uint8_t,
                16>,
            uint16_t,
            std::chrono::milliseconds)
        {
            CO_RETURN descriptor_result{rpc::error::PROTOCOL_ERROR(), 0, 0, 0};
        }

        virtual CORO_TASK(operation_result) set_tcp_no_delay(uint32_t)
        {
            CO_RETURN operation_result{rpc::error::PROTOCOL_ERROR(), 0, 0};
        }

        virtual CORO_TASK(transfer_result) send(
            uint32_t,
            rpc::byte_span,
            uint32_t)
        {
            CO_RETURN transfer_result{rpc::error::PROTOCOL_ERROR(), 0, 0, 0};
        }

        virtual CORO_TASK(transfer_result) receive(
            uint32_t,
            rpc::mutable_byte_span,
            uint32_t)
        {
            CO_RETURN transfer_result{rpc::error::PROTOCOL_ERROR(), 0, 0, 0};
        }

        virtual CORO_TASK(operation_result) cancel_direct(uint32_t)
        {
            CO_RETURN operation_result{rpc::error::OK(), 0, 0};
        }

        virtual CORO_TASK(operation_result) close_direct(uint32_t)
        {
            CO_RETURN operation_result{rpc::error::PROTOCOL_ERROR(), 0, 0};
        }

        virtual void close() noexcept = 0;
    };
} // namespace rpc::io_uring
