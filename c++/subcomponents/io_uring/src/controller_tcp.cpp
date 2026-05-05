/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <io_uring/controller.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

namespace rpc::io_uring
{
    namespace
    {
        static constexpr int32_t socket_family_inet = 2;
        static constexpr uint64_t socket_type_stream = 1;
        static constexpr uint32_t socket_protocol_tcp = 6;
        static constexpr size_t ipv4_sockaddr_size = 16;
        static constexpr int32_t native_operation_cancelled = ECANCELED;
#if defined(ETIME)
        static constexpr int32_t native_timer_expired = ETIME;
#else
        static constexpr int32_t native_timer_expired = 62;
#endif

        detail::kernel_timespec make_kernel_timespec(std::chrono::milliseconds timeout) noexcept
        {
            const auto milliseconds = timeout.count();
            return detail::kernel_timespec{
                static_cast<int64_t>(milliseconds / 1000), static_cast<int64_t>((milliseconds % 1000) * 1'000'000)};
        }

        bool is_linked_timeout_result(int32_t native_result) noexcept
        {
            if (native_result >= 0)
            {
                return false;
            }

            const auto native_error = -native_result;
            return native_error == native_operation_cancelled || native_error == native_timer_expired;
        }
    } // namespace

    // Creates an IPv4 TCP socket directly into the io_uring fixed-file table.
    // The returned descriptor is a direct descriptor index, not a normal process
    // file descriptor visible inside the enclave.
    CORO_TASK(descriptor_result) controller::create_tcp_socket()
    {
        auto fixed_files_err = CO_AWAIT ensure_fixed_file_table();
        if (fixed_files_err != rpc::error::OK())
        {
            CO_RETURN descriptor_result{fixed_files_err, 0, 0, 0};
        }

        auto result = CO_AWAIT submit_operation(
            [](detail::sqe_64& sqe, void*)
            {
                sqe.opcode = detail::io_uring_op_socket;
                sqe.fd = socket_family_inet;
                sqe.off = socket_type_stream;
                sqe.len = socket_protocol_tcp;
                sqe.rw_flags = 0;
                sqe.file_index = detail::io_uring_file_index_alloc;
            },
            nullptr);

        if (result.error_code != rpc::error::OK())
        {
            CO_RETURN descriptor_result{result.error_code, 0, result.native_result, result.cqe_flags};
        }

        CO_RETURN descriptor_result{
            rpc::error::OK(), static_cast<uint32_t>(result.native_result), result.native_result, result.cqe_flags};
    }

    // Binds a direct TCP socket to 127.0.0.1:port. The sockaddr must live in a
    // host-registered buffer because the kernel cannot read enclave-private
    // stack memory for the SQE.
    CORO_TASK(operation_result)
    controller::bind_tcp_ipv4_loopback(
        uint32_t descriptor,
        uint16_t port)
    {
        auto address_buffer_result = CO_AWAIT make_loopback_address_buffer(port);
        if (address_buffer_result.error_code != rpc::error::OK())
        {
            CO_RETURN operation_result{address_buffer_result.error_code, 0, 0};
        }

        struct context
        {
            uint32_t descriptor;
            std::shared_ptr<host_buffer> address_buffer;
        } operation_context{descriptor, std::move(address_buffer_result.buffer)};

        CO_RETURN CO_AWAIT submit_operation(
            [](detail::sqe_64& sqe, void* data)
            {
                auto& operation_context = *static_cast<context*>(data);
                sqe.opcode = detail::io_uring_op_bind;
                sqe.flags = detail::io_uring_sqe_fixed_file;
                sqe.fd = static_cast<int32_t>(operation_context.descriptor);
                sqe.addr = operation_context.address_buffer->address();
                sqe.off = static_cast<uint64_t>(ipv4_sockaddr_size);
            },
            &operation_context);
    }

    // Puts a bound direct TCP socket into listen mode using IORING_OP_LISTEN.
    CORO_TASK(operation_result)
    controller::listen(
        uint32_t descriptor,
        uint32_t backlog)
    {
        struct context
        {
            uint32_t descriptor;
            uint32_t backlog;
        } operation_context{descriptor, backlog};

        CO_RETURN CO_AWAIT submit_operation(
            [](detail::sqe_64& sqe, void* data)
            {
                auto& operation_context = *static_cast<context*>(data);
                sqe.opcode = detail::io_uring_op_listen;
                sqe.flags = detail::io_uring_sqe_fixed_file;
                sqe.fd = static_cast<int32_t>(operation_context.descriptor);
                sqe.len = operation_context.backlog;
            },
            &operation_context);
    }

    // Accepts one inbound connection from a listening direct descriptor and asks
    // the kernel to allocate the accepted socket into the same fixed-file table.
    CORO_TASK(descriptor_result) controller::accept(uint32_t listen_descriptor)
    {
        auto fixed_files_err = CO_AWAIT ensure_fixed_file_table();
        if (fixed_files_err != rpc::error::OK())
        {
            CO_RETURN descriptor_result{fixed_files_err, 0, 0, 0};
        }

        struct context
        {
            uint32_t listen_descriptor;
        } operation_context{listen_descriptor};

        auto result = CO_AWAIT submit_operation(
            [](detail::sqe_64& sqe, void* data)
            {
                auto& operation_context = *static_cast<context*>(data);
                sqe.opcode = detail::io_uring_op_accept;
                sqe.flags = detail::io_uring_sqe_fixed_file;
                sqe.fd = static_cast<int32_t>(operation_context.listen_descriptor);
                sqe.file_index = detail::io_uring_file_index_alloc;
            },
            &operation_context);

        if (result.error_code != rpc::error::OK())
        {
            CO_RETURN descriptor_result{result.error_code, 0, result.native_result, result.cqe_flags};
        }

        CO_RETURN descriptor_result{
            rpc::error::OK(), static_cast<uint32_t>(result.native_result), result.native_result, result.cqe_flags};
    }

    // Creates a direct TCP socket and connects it to 127.0.0.1:port. If the
    // connect setup fails after socket creation, the direct descriptor is closed.
    CORO_TASK(descriptor_result)
    controller::connect_tcp_ipv4_loopback(uint16_t port)
    {
        auto socket_result = CO_AWAIT create_tcp_socket();
        if (socket_result.error_code != rpc::error::OK())
        {
            CO_RETURN socket_result;
        }

        auto address_buffer_result = CO_AWAIT make_loopback_address_buffer(port);
        if (address_buffer_result.error_code != rpc::error::OK())
        {
            CO_AWAIT close_direct(socket_result.descriptor);
            CO_RETURN descriptor_result{address_buffer_result.error_code, 0, 0, 0};
        }

        struct context
        {
            uint32_t descriptor;
            std::shared_ptr<host_buffer> address_buffer;
        } operation_context{socket_result.descriptor, std::move(address_buffer_result.buffer)};

        auto connect_result = CO_AWAIT submit_operation(
            [](detail::sqe_64& sqe, void* data)
            {
                auto& operation_context = *static_cast<context*>(data);
                sqe.opcode = detail::io_uring_op_connect;
                sqe.flags = detail::io_uring_sqe_fixed_file;
                sqe.fd = static_cast<int32_t>(operation_context.descriptor);
                sqe.addr = operation_context.address_buffer->address();
                sqe.off = static_cast<uint64_t>(ipv4_sockaddr_size);
            },
            &operation_context);

        if (connect_result.error_code != rpc::error::OK())
        {
            CO_AWAIT close_direct(socket_result.descriptor);
            CO_RETURN descriptor_result{
                connect_result.error_code, 0, connect_result.native_result, connect_result.cqe_flags};
        }

        CO_RETURN descriptor_result{
            rpc::error::OK(), socket_result.descriptor, connect_result.native_result, connect_result.cqe_flags};
    }

    // Copies bytes from enclave memory into a host-registered buffer, then
    // submits IORING_OP_SEND against a direct TCP descriptor.
    CORO_TASK(transfer_result)
    controller::send(
        uint32_t descriptor,
        rpc::byte_span buffer)
    {
        if (buffer.empty())
        {
            CO_RETURN transfer_result{rpc::error::OK(), 0, 0, 0};
        }

        auto buffer_result = CO_AWAIT allocate_host_buffer(buffer.size());
        if (buffer_result.error_code != rpc::error::OK())
        {
            CO_RETURN transfer_result{buffer_result.error_code, 0, 0, 0};
        }

        struct context
        {
            uint32_t descriptor;
            std::shared_ptr<host_buffer> buffer;
        } operation_context{descriptor, std::move(buffer_result.buffer)};

        std::memcpy(operation_context.buffer->data(), buffer.data(), operation_context.buffer->size());
        auto result = CO_AWAIT submit_operation(
            [](detail::sqe_64& sqe, void* data)
            {
                auto& operation_context = *static_cast<context*>(data);
                sqe.opcode = detail::io_uring_op_send;
                sqe.flags = detail::io_uring_sqe_fixed_file;
                sqe.fd = static_cast<int32_t>(operation_context.descriptor);
                sqe.addr = operation_context.buffer->address();
                sqe.len = static_cast<uint32_t>(operation_context.buffer->size());
                sqe.msg_flags = 0;
            },
            &operation_context);

        CO_RETURN transfer_result{result.error_code,
            result.native_result > 0 ? static_cast<uint32_t>(result.native_result) : 0U,
            result.native_result,
            result.cqe_flags};
    }

    // Submits IORING_OP_RECV into a host-registered buffer, then copies the
    // completed bytes back into the caller's enclave buffer.
    CORO_TASK(transfer_result)
    controller::receive(
        uint32_t descriptor,
        rpc::mutable_byte_span buffer)
    {
        if (buffer.empty())
        {
            CO_RETURN transfer_result{rpc::error::OK(), 0, 0, 0};
        }

        auto buffer_result = CO_AWAIT allocate_host_buffer(buffer.size());
        if (buffer_result.error_code != rpc::error::OK())
        {
            CO_RETURN transfer_result{buffer_result.error_code, 0, 0, 0};
        }

        struct context
        {
            uint32_t descriptor;
            std::shared_ptr<host_buffer> buffer;
        } operation_context{descriptor, std::move(buffer_result.buffer)};

        auto result = CO_AWAIT submit_operation(
            [](detail::sqe_64& sqe, void* data)
            {
                auto& operation_context = *static_cast<context*>(data);
                sqe.opcode = detail::io_uring_op_recv;
                sqe.flags = detail::io_uring_sqe_fixed_file;
                sqe.fd = static_cast<int32_t>(operation_context.descriptor);
                sqe.addr = operation_context.buffer->address();
                sqe.len = static_cast<uint32_t>(operation_context.buffer->size());
                sqe.msg_flags = 0;
            },
            &operation_context);

        uint32_t bytes_transferred = 0;
        if (result.error_code == rpc::error::OK() && result.native_result > 0)
        {
            bytes_transferred = static_cast<uint32_t>(result.native_result);
            std::memcpy(buffer.data(), operation_context.buffer->data(), bytes_transferred);
        }

        CO_RETURN transfer_result{result.error_code, bytes_transferred, result.native_result, result.cqe_flags};
    }

    // Receives into a host-registered buffer with a kernel-enforced timeout.
    // The RECV SQE is linked to IORING_OP_LINK_TIMEOUT so the kernel cancels
    // the receive if no bytes arrive before the deadline. The timeout structure
    // also lives in a host buffer because the kernel cannot read enclave stack
    // memory.
    CORO_TASK(transfer_result)
    controller::receive(
        uint32_t descriptor,
        rpc::mutable_byte_span buffer,
        std::chrono::milliseconds timeout)
    {
        if (timeout <= std::chrono::milliseconds{0})
        {
            CO_RETURN CO_AWAIT receive(descriptor, buffer);
        }

        if (buffer.empty())
        {
            CO_RETURN transfer_result{rpc::error::OK(), 0, 0, 0};
        }

        auto buffer_pair_result = CO_AWAIT allocate_host_buffer_pair(buffer.size(), sizeof(detail::kernel_timespec));
        if (buffer_pair_result.error_code != rpc::error::OK())
        {
            CO_RETURN transfer_result{buffer_pair_result.error_code, 0, 0, 0};
        }

        auto timeout_spec = make_kernel_timespec(timeout);
        std::memcpy(buffer_pair_result.second_buffer->data(), &timeout_spec, sizeof(timeout_spec));

        struct context
        {
            uint32_t descriptor;
            std::shared_ptr<host_buffer> buffer;
            std::shared_ptr<host_buffer> timeout_buffer;
        };
        context operation_context{
            descriptor, std::move(buffer_pair_result.first_buffer), std::move(buffer_pair_result.second_buffer)};

        auto result = CO_AWAIT submit_linked_operation(
            [](detail::sqe_64& recv_sqe, detail::sqe_64& timeout_sqe, void* data)
            {
                auto& operation_context = *static_cast<context*>(data);
                recv_sqe.opcode = detail::io_uring_op_recv;
                recv_sqe.flags = detail::io_uring_sqe_fixed_file | detail::io_uring_sqe_io_link;
                recv_sqe.fd = static_cast<int32_t>(operation_context.descriptor);
                recv_sqe.addr = operation_context.buffer->address();
                recv_sqe.len = static_cast<uint32_t>(operation_context.buffer->size());
                recv_sqe.msg_flags = 0;

                timeout_sqe.opcode = detail::io_uring_op_link_timeout;
                timeout_sqe.addr = operation_context.timeout_buffer->address();
                timeout_sqe.len = 1;
            },
            &operation_context,
            operation_context.timeout_buffer);

        if (result.error_code != rpc::error::OK() && is_linked_timeout_result(result.native_result))
        {
            CO_RETURN transfer_result{rpc::error::CALL_TIMEOUT(), 0, result.native_result, result.cqe_flags};
        }

        uint32_t bytes_transferred = 0;
        if (result.error_code == rpc::error::OK() && result.native_result > 0)
        {
            bytes_transferred = static_cast<uint32_t>(result.native_result);
            std::memcpy(buffer.data(), operation_context.buffer->data(), bytes_transferred);
        }

        CO_RETURN transfer_result{result.error_code, bytes_transferred, result.native_result, result.cqe_flags};
    }

    // Cancels pending io_uring work that targets one direct descriptor. This is
    // used before close so a blocking accept/recv/send waiter is completed by
    // the kernel instead of being left pending on a fixed-file slot that is
    // about to be closed.
    CORO_TASK(operation_result) controller::cancel_direct(uint32_t descriptor)
    {
        if (descriptor == std::numeric_limits<uint32_t>::max())
        {
            CO_RETURN operation_result{rpc::error::INVALID_DATA(), 0, 0};
        }

        struct context
        {
            uint32_t descriptor;
        } operation_context{descriptor};

        CO_RETURN CO_AWAIT submit_operation(
            [](detail::sqe_64& sqe, void* data)
            {
                auto& operation_context = *static_cast<context*>(data);
                sqe.opcode = detail::io_uring_op_async_cancel;
                sqe.fd = static_cast<int32_t>(operation_context.descriptor);
                sqe.rw_flags = detail::io_uring_async_cancel_all | detail::io_uring_async_cancel_fd
                               | detail::io_uring_async_cancel_fd_fixed;
            },
            &operation_context);
    }

    // Closes a direct descriptor by submitting an io_uring close operation that
    // targets the fixed-file table slot rather than a normal fd value.
    CORO_TASK(operation_result) controller::close_direct(uint32_t descriptor)
    {
        if (descriptor == std::numeric_limits<uint32_t>::max())
        {
            CO_RETURN operation_result{rpc::error::INVALID_DATA(), 0, 0};
        }

        struct context
        {
            uint32_t descriptor;
        } operation_context{descriptor};

        CO_RETURN CO_AWAIT submit_operation(
            [](detail::sqe_64& sqe, void* data)
            {
                auto& operation_context = *static_cast<context*>(data);
                sqe.opcode = detail::io_uring_op_close;
                sqe.file_index = operation_context.descriptor + 1U;
            },
            &operation_context);
    }
} // namespace rpc::io_uring
