/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <io_uring/controller.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <utility>

namespace rpc::io_uring
{
    namespace
    {
        static constexpr int32_t socket_family_inet = 2;
        static constexpr int32_t socket_family_inet6 = 10;
        static constexpr uint64_t socket_type_stream = 1;
        static constexpr uint32_t socket_protocol_tcp = 6;
        static constexpr uint32_t socket_level_socket = 1;
        static constexpr uint32_t socket_level_tcp = socket_protocol_tcp;
        static constexpr uint32_t socket_option_reuse_addr = 2;
        static constexpr uint32_t socket_option_tcp_no_delay = 1;
        static constexpr uint32_t socket_message_dontwait = 0x40;
        static constexpr size_t ipv4_sockaddr_size = 16;
        static constexpr size_t ipv6_sockaddr_size = 28;
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

        bool is_retryable_socket_result(int32_t native_result) noexcept
        {
            if (native_result >= 0)
            {
                return false;
            }

            const auto native_error = -native_result;
            return native_error == EAGAIN || native_error == EWOULDBLOCK || native_error == EINPROGRESS;
        }

        uint64_t user_pointer_value(const void* ptr) noexcept
        {
            return static_cast<uint64_t>(reinterpret_cast<std::uintptr_t>(ptr));
        }

        uint32_t clamped_transfer_size(size_t size) noexcept
        {
            const auto max_transfer_size = static_cast<size_t>(std::numeric_limits<uint32_t>::max());
            return static_cast<uint32_t>(size > max_transfer_size ? max_transfer_size : size);
        }
    } // namespace

    // TCP data movement in the direct controller
    // ------------------------------------------
    //
    // Socket creation, bind, listen, accept, connect, send and receive all use
    // SQEs written by controller_submission.cpp. The TCP-specific code here is
    // mostly responsible for choosing the right kernel-visible pointers:
    //
    //     bind/connect: sockaddr      -> staging buffer -> SQE.addr
    //     setsockopt:   int option    -> staging buffer -> SQE.optval
    //     send:         caller bytes  -> staging buffer -> SQE.addr
    //     recv:         SQE.addr      -> staging buffer -> caller bytes
    //
    // Host builds may skip staging for transfer buffers when
    // use_caller_buffers_for_transfers is enabled.

    // Creates a TCP socket directly into the io_uring fixed-file table. The
    // returned descriptor is a direct descriptor index, not a normal process
    // file descriptor.
    CORO_TASK(descriptor_result) controller::create_tcp_ipv4_socket()
    {
        auto err = CO_AWAIT ensure_iouring_data();
        if (err != rpc::error::OK())
        {
            CO_RETURN descriptor_result{err, 0, 0, 0};
        }
        if (!cached_fixed_file_table_available())
        {
            if (handle_)
                CO_RETURN CO_AWAIT handle_->create_tcp_ipv4_socket();
            CO_RETURN descriptor_result{rpc::error::PROTOCOL_ERROR(), 0, 0, 0};
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

        // TCP_NODELAY is best-effort only. In direct fixed-file mode the
        // setsockopt URING_CMD also needs a staging buffer, so doing it here can
        // block connection establishment under deliberately tiny staging pools.
        CO_RETURN descriptor_result{
            rpc::error::OK(), static_cast<uint32_t>(result.native_result), result.native_result, result.cqe_flags};
    }

    CORO_TASK(descriptor_result) controller::create_tcp_ipv6_socket()
    {
        auto err = CO_AWAIT ensure_iouring_data();
        if (err != rpc::error::OK())
        {
            CO_RETURN descriptor_result{err, 0, 0, 0};
        }
        if (!cached_fixed_file_table_available())
        {
            if (handle_)
                CO_RETURN CO_AWAIT handle_->create_tcp_ipv6_socket();
            CO_RETURN descriptor_result{rpc::error::PROTOCOL_ERROR(), 0, 0, 0};
        }

        auto result = CO_AWAIT submit_operation(
            [](detail::sqe_64& sqe, void*)
            {
                sqe.opcode = detail::io_uring_op_socket;
                sqe.fd = socket_family_inet6;
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

    // Allows an io_uring direct TCP listener to re-bind a recently used local
    // address. The socket is only available as a fixed-file index, so issue
    // SO_REUSEADDR through SOCKET_URING_OP_SETSOCKOPT before bind().
    CORO_TASK(operation_result) controller::set_socket_reuse_addr(uint32_t descriptor)
    {
        auto err = CO_AWAIT ensure_iouring_data();
        if (err != rpc::error::OK())
        {
            CO_RETURN operation_result{err, 0, 0};
        }
        if (!cached_fixed_file_table_available())
        {
            if (handle_)
                CO_RETURN CO_AWAIT handle_->set_socket_reuse_addr(descriptor);
            CO_RETURN operation_result{rpc::error::PROTOCOL_ERROR(), 0, 0};
        }

        auto option_buffer_result = CO_AWAIT allocate_staging_buffer(sizeof(int));
        if (option_buffer_result.error_code != rpc::error::OK())
        {
            CO_RETURN operation_result{option_buffer_result.error_code, 0, 0};
        }

        const int option_value = 1;
        std::memcpy(option_buffer_result.buffer->data(), &option_value, sizeof(option_value));

        struct context
        {
            uint32_t descriptor;
            std::shared_ptr<staging_buffer> option_buffer;
        } operation_context{descriptor, std::move(option_buffer_result.buffer)};

        CO_RETURN CO_AWAIT submit_operation(
            [](detail::sqe_64& sqe, void* data)
            {
                auto& operation_context = *static_cast<context*>(data);
                sqe.opcode = detail::io_uring_op_uring_cmd;
                sqe.flags = detail::io_uring_sqe_fixed_file;
                sqe.fd = static_cast<int32_t>(operation_context.descriptor);
                sqe.command.cmd_op = detail::socket_uring_op_setsockopt;
                sqe.socket_option.level = socket_level_socket;
                sqe.socket_option.optname = socket_option_reuse_addr;
                sqe.optlen = sizeof(int);
                sqe.optval = operation_context.option_buffer->address();
            },
            &operation_context);
    }

    // Binds a direct TCP socket to 127.0.0.1:port. The sockaddr lives in a
    // ring-visible staging buffer so the kernel sees stable memory for the SQE.
    CORO_TASK(operation_result)
    controller::bind_tcp_ipv4_loopback(
        uint32_t descriptor,
        uint16_t port)
    {
        auto err = CO_AWAIT ensure_iouring_data();
        if (err != rpc::error::OK())
        {
            CO_RETURN operation_result{err, 0, 0};
        }
        if (!cached_fixed_file_table_available())
        {
            if (handle_)
                CO_RETURN CO_AWAIT handle_->bind_tcp_ipv4_loopback(descriptor, port);
            CO_RETURN operation_result{rpc::error::PROTOCOL_ERROR(), 0, 0};
        }

        CO_RETURN CO_AWAIT bind_tcp_ipv4(descriptor, {127, 0, 0, 1}, port);
    }

    CORO_TASK(operation_result)
    controller::bind_tcp_ipv4(
        uint32_t descriptor,
        const std::array<
            uint8_t,
            4>& bind_address,
        uint16_t port)
    {
        auto err = CO_AWAIT ensure_iouring_data();
        if (err != rpc::error::OK())
        {
            CO_RETURN operation_result{err, 0, 0};
        }
        if (!cached_fixed_file_table_available())
        {
            if (handle_)
                CO_RETURN CO_AWAIT handle_->bind_tcp_ipv4(descriptor, bind_address, port);
            CO_RETURN operation_result{rpc::error::PROTOCOL_ERROR(), 0, 0};
        }

        auto address_buffer_result = CO_AWAIT make_ipv4_address_buffer(bind_address, port);
        if (address_buffer_result.error_code != rpc::error::OK())
        {
            CO_RETURN operation_result{address_buffer_result.error_code, 0, 0};
        }

        struct context
        {
            uint32_t descriptor;
            std::shared_ptr<staging_buffer> address_buffer;
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

    CORO_TASK(operation_result)
    controller::bind_tcp_ipv6(
        uint32_t descriptor,
        const std::array<
            uint8_t,
            16>& bind_address,
        uint16_t port)
    {
        auto err = CO_AWAIT ensure_iouring_data();
        if (err != rpc::error::OK())
        {
            CO_RETURN operation_result{err, 0, 0};
        }
        if (!cached_fixed_file_table_available())
        {
            if (handle_)
                CO_RETURN CO_AWAIT handle_->bind_tcp_ipv6(descriptor, bind_address, port);
            CO_RETURN operation_result{rpc::error::PROTOCOL_ERROR(), 0, 0};
        }

        auto address_buffer_result = CO_AWAIT make_ipv6_address_buffer(bind_address, port);
        if (address_buffer_result.error_code != rpc::error::OK())
        {
            CO_RETURN operation_result{address_buffer_result.error_code, 0, 0};
        }

        struct context
        {
            uint32_t descriptor;
            std::shared_ptr<staging_buffer> address_buffer;
        } operation_context{descriptor, std::move(address_buffer_result.buffer)};

        CO_RETURN CO_AWAIT submit_operation(
            [](detail::sqe_64& sqe, void* data)
            {
                auto& operation_context = *static_cast<context*>(data);
                sqe.opcode = detail::io_uring_op_bind;
                sqe.flags = detail::io_uring_sqe_fixed_file;
                sqe.fd = static_cast<int32_t>(operation_context.descriptor);
                sqe.addr = operation_context.address_buffer->address();
                sqe.off = static_cast<uint64_t>(ipv6_sockaddr_size);
            },
            &operation_context);
    }

    // Puts a bound direct TCP socket into listen mode using IORING_OP_LISTEN.
    CORO_TASK(operation_result)
    controller::listen(
        uint32_t descriptor,
        uint32_t backlog)
    {
        auto err = CO_AWAIT ensure_iouring_data();
        if (err != rpc::error::OK())
        {
            CO_RETURN operation_result{err, 0, 0};
        }
        if (!cached_fixed_file_table_available())
        {
            if (handle_)
                CO_RETURN CO_AWAIT handle_->listen(descriptor, backlog);
            CO_RETURN operation_result{rpc::error::PROTOCOL_ERROR(), 0, 0};
        }

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

    // Disables Nagle for a direct TCP descriptor. The descriptor is a fixed-file
    // table index, so ordinary setsockopt() cannot see it; use the kernel socket
    // URING_CMD operation and keep the option value in ring-visible memory.
    CORO_TASK(operation_result) controller::set_tcp_no_delay(uint32_t descriptor)
    {
        auto err = CO_AWAIT ensure_iouring_data();
        if (err != rpc::error::OK())
        {
            CO_RETURN operation_result{err, 0, 0};
        }
        if (!cached_fixed_file_table_available())
        {
            if (handle_)
                CO_RETURN CO_AWAIT handle_->set_tcp_no_delay(descriptor);
            CO_RETURN operation_result{rpc::error::PROTOCOL_ERROR(), 0, 0};
        }

        auto option_buffer_result = CO_AWAIT allocate_staging_buffer(sizeof(int));
        if (option_buffer_result.error_code != rpc::error::OK())
        {
            CO_RETURN operation_result{option_buffer_result.error_code, 0, 0};
        }

        const int option_value = 1;
        std::memcpy(option_buffer_result.buffer->data(), &option_value, sizeof(option_value));

        struct context
        {
            uint32_t descriptor;
            std::shared_ptr<staging_buffer> option_buffer;
        } operation_context{descriptor, std::move(option_buffer_result.buffer)};

        CO_RETURN CO_AWAIT submit_operation(
            [](detail::sqe_64& sqe, void* data)
            {
                auto& operation_context = *static_cast<context*>(data);
                sqe.opcode = detail::io_uring_op_uring_cmd;
                sqe.flags = detail::io_uring_sqe_fixed_file;
                sqe.fd = static_cast<int32_t>(operation_context.descriptor);
                sqe.command.cmd_op = detail::socket_uring_op_setsockopt;
                sqe.socket_option.level = socket_level_tcp;
                sqe.socket_option.optname = socket_option_tcp_no_delay;
                sqe.optlen = sizeof(int);
                sqe.optval = operation_context.option_buffer->address();
            },
            &operation_context);
    }

    // Accepts one inbound connection from a listening direct descriptor and asks
    // the kernel to allocate the accepted socket into the same fixed-file table.
    CORO_TASK(descriptor_result) controller::accept(uint32_t listen_descriptor)
    {
        auto err = CO_AWAIT ensure_iouring_data();
        if (err != rpc::error::OK())
        {
            CO_RETURN descriptor_result{err, 0, 0, 0};
        }
        if (!cached_fixed_file_table_available())
        {
            if (!handle_)
            {
                CO_RETURN descriptor_result{rpc::error::PROTOCOL_ERROR(), 0, 0, 0};
            }

            while (can_accept_work())
            {
                auto result = CO_AWAIT handle_->accept(listen_descriptor);
                if (result.error_code == rpc::error::OK())
                {
                    auto no_delay_result = CO_AWAIT set_tcp_no_delay(result.descriptor);
                    if (no_delay_result.error_code != rpc::error::OK())
                    {
                        RPC_WARNING(
                            "descriptor fallback TCP_NODELAY failed for accepted descriptor={} error_code={} "
                            "native_result={}",
                            result.descriptor,
                            no_delay_result.error_code,
                            no_delay_result.native_result);
                    }
                    CO_RETURN result;
                }

                if (!is_retryable_socket_result(result.native_result))
                {
                    CO_RETURN result;
                }

                CO_AWAIT wait_before_next_poll();
            }

            CO_RETURN descriptor_result{shutdown_error(), 0, 0, 0};
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

    CORO_TASK(descriptor_result)
    controller::connect_tcp_ipv4_loopback(
        uint16_t port,
        std::chrono::milliseconds timeout)
    {
        CO_RETURN CO_AWAIT connect_tcp_ipv4({127, 0, 0, 1}, port, timeout);
    }

    CORO_TASK(descriptor_result)
    controller::connect_tcp_ipv4(
        const std::array<
            uint8_t,
            4>& address,
        uint16_t port,
        std::chrono::milliseconds timeout)
    {
        auto err = CO_AWAIT ensure_iouring_data();
        if (err != rpc::error::OK())
        {
            CO_RETURN descriptor_result{err, 0, 0, 0};
        }
        if (!cached_fixed_file_table_available())
        {
            if (!handle_)
            {
                CO_RETURN descriptor_result{rpc::error::PROTOCOL_ERROR(), 0, 0, 0};
            }

            auto connect_result = CO_AWAIT handle_->connect_tcp_ipv4(address, port, timeout);
            if (connect_result.error_code != rpc::error::OK())
            {
                CO_RETURN connect_result;
            }

            auto no_delay_result = CO_AWAIT set_tcp_no_delay(connect_result.descriptor);
            if (no_delay_result.error_code != rpc::error::OK())
            {
                RPC_WARNING(
                    "descriptor fallback TCP_NODELAY failed for connected descriptor={} error_code={} native_result={}",
                    connect_result.descriptor,
                    no_delay_result.error_code,
                    no_delay_result.native_result);
            }

            CO_RETURN connect_result;
        }

        auto socket_result = CO_AWAIT create_tcp_ipv4_socket();
        if (socket_result.error_code != rpc::error::OK())
        {
            CO_RETURN socket_result;
        }

        operation_result connect_result;
        {
            // Keep the staged sockaddr and optional timeout scoped to just the
            // connect SQE. Once submit_linked_operation/submit_operation
            // returns, the kernel has completed the operation and the buffers
            // can be released before optional post-connect work.
            auto address_buffer_result = CO_AWAIT make_ipv4_address_buffer(address, port);
            if (address_buffer_result.error_code != rpc::error::OK())
            {
                CO_AWAIT close_direct(socket_result.descriptor);
                CO_RETURN descriptor_result{address_buffer_result.error_code, 0, 0, 0};
            }

            struct context
            {
                uint32_t descriptor;
                std::shared_ptr<staging_buffer> address_buffer;
                std::shared_ptr<staging_buffer> timeout_buffer;
                uint64_t address_size;
            } operation_context{socket_result.descriptor, std::move(address_buffer_result.buffer), {}, ipv4_sockaddr_size};

            if (timeout > std::chrono::milliseconds{0})
            {
                auto timeout_buffer_result = CO_AWAIT allocate_staging_buffer(sizeof(detail::kernel_timespec));
                if (timeout_buffer_result.error_code != rpc::error::OK())
                {
                    CO_AWAIT close_direct(socket_result.descriptor);
                    CO_RETURN descriptor_result{timeout_buffer_result.error_code, 0, 0, 0};
                }

                auto timeout_spec = make_kernel_timespec(timeout);
                std::memcpy(timeout_buffer_result.buffer->data(), &timeout_spec, sizeof(timeout_spec));
                operation_context.timeout_buffer = std::move(timeout_buffer_result.buffer);

                connect_result = CO_AWAIT submit_linked_operation(
                    [](detail::sqe_64& connect_sqe, detail::sqe_64& timeout_sqe, void* data)
                    {
                        auto& operation_context = *static_cast<context*>(data);
                        connect_sqe.opcode = detail::io_uring_op_connect;
                        connect_sqe.flags = detail::io_uring_sqe_fixed_file | detail::io_uring_sqe_io_link;
                        connect_sqe.fd = static_cast<int32_t>(operation_context.descriptor);
                        connect_sqe.addr = operation_context.address_buffer->address();
                        connect_sqe.off = operation_context.address_size;

                        timeout_sqe.opcode = detail::io_uring_op_link_timeout;
                        timeout_sqe.addr = operation_context.timeout_buffer->address();
                        timeout_sqe.len = 1;
                    },
                    &operation_context,
                    operation_context.timeout_buffer);
            }
            else
            {
                connect_result = CO_AWAIT submit_operation(
                    [](detail::sqe_64& sqe, void* data)
                    {
                        auto& operation_context = *static_cast<context*>(data);
                        sqe.opcode = detail::io_uring_op_connect;
                        sqe.flags = detail::io_uring_sqe_fixed_file;
                        sqe.fd = static_cast<int32_t>(operation_context.descriptor);
                        sqe.addr = operation_context.address_buffer->address();
                        sqe.off = operation_context.address_size;
                    },
                    &operation_context);
            }
        }

        if (connect_result.error_code != rpc::error::OK())
        {
            CO_AWAIT close_direct(socket_result.descriptor);
            if (is_linked_timeout_result(connect_result.native_result))
            {
                CO_RETURN descriptor_result{
                    rpc::error::CALL_TIMEOUT(), 0, connect_result.native_result, connect_result.cqe_flags};
            }
            CO_RETURN descriptor_result{
                connect_result.error_code, 0, connect_result.native_result, connect_result.cqe_flags};
        }

        // The direct fixed-file path avoids optional TCP_NODELAY here because
        // the staged setsockopt can starve real connect/send/receive work under
        // constrained staging buffer pools.
        CO_RETURN descriptor_result{
            rpc::error::OK(), socket_result.descriptor, connect_result.native_result, connect_result.cqe_flags};
    }

    CORO_TASK(descriptor_result)
    controller::connect_tcp_ipv6(
        const std::array<
            uint8_t,
            16>& address,
        uint16_t port,
        std::chrono::milliseconds timeout)
    {
        auto err = CO_AWAIT ensure_iouring_data();
        if (err != rpc::error::OK())
        {
            CO_RETURN descriptor_result{err, 0, 0, 0};
        }
        if (!cached_fixed_file_table_available())
        {
            if (!handle_)
            {
                CO_RETURN descriptor_result{rpc::error::PROTOCOL_ERROR(), 0, 0, 0};
            }

            auto connect_result = CO_AWAIT handle_->connect_tcp_ipv6(address, port, timeout);
            if (connect_result.error_code != rpc::error::OK())
            {
                CO_RETURN connect_result;
            }

            auto no_delay_result = CO_AWAIT set_tcp_no_delay(connect_result.descriptor);
            if (no_delay_result.error_code != rpc::error::OK())
            {
                RPC_WARNING(
                    "descriptor fallback TCP_NODELAY failed for connected descriptor={} error_code={} native_result={}",
                    connect_result.descriptor,
                    no_delay_result.error_code,
                    no_delay_result.native_result);
            }

            CO_RETURN connect_result;
        }

        auto socket_result = CO_AWAIT create_tcp_ipv6_socket();
        if (socket_result.error_code != rpc::error::OK())
        {
            CO_RETURN socket_result;
        }

        operation_result connect_result;
        {
            // See the IPv4 connect path above. The scoped block is intentional:
            // staged address/timeout buffers are scarce in small pools and
            // should not be held after the connect CQE has been consumed.
            auto address_buffer_result = CO_AWAIT make_ipv6_address_buffer(address, port);
            if (address_buffer_result.error_code != rpc::error::OK())
            {
                CO_AWAIT close_direct(socket_result.descriptor);
                CO_RETURN descriptor_result{address_buffer_result.error_code, 0, 0, 0};
            }

            struct context
            {
                uint32_t descriptor;
                std::shared_ptr<staging_buffer> address_buffer;
                std::shared_ptr<staging_buffer> timeout_buffer;
                uint64_t address_size;
            } operation_context{socket_result.descriptor, std::move(address_buffer_result.buffer), {}, ipv6_sockaddr_size};

            if (timeout > std::chrono::milliseconds{0})
            {
                auto timeout_buffer_result = CO_AWAIT allocate_staging_buffer(sizeof(detail::kernel_timespec));
                if (timeout_buffer_result.error_code != rpc::error::OK())
                {
                    CO_AWAIT close_direct(socket_result.descriptor);
                    CO_RETURN descriptor_result{timeout_buffer_result.error_code, 0, 0, 0};
                }

                auto timeout_spec = make_kernel_timespec(timeout);
                std::memcpy(timeout_buffer_result.buffer->data(), &timeout_spec, sizeof(timeout_spec));
                operation_context.timeout_buffer = std::move(timeout_buffer_result.buffer);

                connect_result = CO_AWAIT submit_linked_operation(
                    [](detail::sqe_64& connect_sqe, detail::sqe_64& timeout_sqe, void* data)
                    {
                        auto& operation_context = *static_cast<context*>(data);
                        connect_sqe.opcode = detail::io_uring_op_connect;
                        connect_sqe.flags = detail::io_uring_sqe_fixed_file | detail::io_uring_sqe_io_link;
                        connect_sqe.fd = static_cast<int32_t>(operation_context.descriptor);
                        connect_sqe.addr = operation_context.address_buffer->address();
                        connect_sqe.off = operation_context.address_size;

                        timeout_sqe.opcode = detail::io_uring_op_link_timeout;
                        timeout_sqe.addr = operation_context.timeout_buffer->address();
                        timeout_sqe.len = 1;
                    },
                    &operation_context,
                    operation_context.timeout_buffer);
            }
            else
            {
                connect_result = CO_AWAIT submit_operation(
                    [](detail::sqe_64& sqe, void* data)
                    {
                        auto& operation_context = *static_cast<context*>(data);
                        sqe.opcode = detail::io_uring_op_connect;
                        sqe.flags = detail::io_uring_sqe_fixed_file;
                        sqe.fd = static_cast<int32_t>(operation_context.descriptor);
                        sqe.addr = operation_context.address_buffer->address();
                        sqe.off = operation_context.address_size;
                    },
                    &operation_context);
            }
        }

        if (connect_result.error_code != rpc::error::OK())
        {
            CO_AWAIT close_direct(socket_result.descriptor);
            if (is_linked_timeout_result(connect_result.native_result))
            {
                CO_RETURN descriptor_result{
                    rpc::error::CALL_TIMEOUT(), 0, connect_result.native_result, connect_result.cqe_flags};
            }
            CO_RETURN descriptor_result{
                connect_result.error_code, 0, connect_result.native_result, connect_result.cqe_flags};
        }

        // The direct fixed-file path avoids optional TCP_NODELAY here because
        // the staged setsockopt can starve real connect/send/receive work under
        // constrained staging buffer pools.
        CO_RETURN descriptor_result{
            rpc::error::OK(), socket_result.descriptor, connect_result.native_result, connect_result.cqe_flags};
    }

    // Sends bytes from the caller's span. Direct submission can use the caller
    // pointer directly when enabled; otherwise the data stages through the
    // controller buffer pool.
    CORO_TASK(transfer_result)
    controller::send(
        uint32_t descriptor,
        rpc::byte_span buffer)
    {
        CO_RETURN CO_AWAIT send_with_flags(descriptor, buffer, options_.send_message_flags);
    }

    CORO_TASK(transfer_result)
    controller::send_with_flags(
        uint32_t descriptor,
        rpc::byte_span buffer,
        uint32_t msg_flags)
    {
        if (buffer.empty())
        {
            CO_RETURN transfer_result{rpc::error::OK(), 0, 0, 0};
        }

        auto err = CO_AWAIT ensure_iouring_data();
        if (err != rpc::error::OK())
        {
            CO_RETURN transfer_result{err, 0, 0, 0};
        }
        if (!cached_fixed_file_table_available())
        {
            if (!handle_)
            {
                CO_RETURN transfer_result{rpc::error::PROTOCOL_ERROR(), 0, 0, 0};
            }

            while (can_accept_work())
            {
                auto result = CO_AWAIT handle_->send(descriptor, buffer, msg_flags);
                if (result.error_code == rpc::error::OK() || !is_retryable_socket_result(result.native_result))
                {
                    CO_RETURN result;
                }

                CO_AWAIT wait_before_next_poll();
            }

            CO_RETURN transfer_result{shutdown_error(), 0, 0, 0};
        }

        if (options_.use_caller_buffers_for_transfers)
        {
            // Fast path:
            //
            //     caller bytes ----------------------> SQE.addr
            const auto transfer_size = clamped_transfer_size(buffer.size());
            auto transfer_buffer = buffer.subspan(0, transfer_size);

            struct context
            {
                uint32_t descriptor;
                rpc::byte_span buffer;
                uint32_t msg_flags;
            } operation_context{descriptor, transfer_buffer, msg_flags};

            auto result = CO_AWAIT submit_operation(
                [](detail::sqe_64& sqe, void* data)
                {
                    auto& operation_context = *static_cast<context*>(data);
                    sqe.opcode = detail::io_uring_op_send;
                    sqe.flags = detail::io_uring_sqe_fixed_file;
                    sqe.fd = static_cast<int32_t>(operation_context.descriptor);
                    sqe.addr = user_pointer_value(operation_context.buffer.data());
                    sqe.len = static_cast<uint32_t>(operation_context.buffer.size());
                    sqe.msg_flags = operation_context.msg_flags;
                },
                &operation_context);

            CO_RETURN transfer_result{result.error_code,
                result.native_result > 0 ? static_cast<uint32_t>(result.native_result) : 0U,
                result.native_result,
                result.cqe_flags};
        }

        auto buffer_result = CO_AWAIT allocate_staging_buffer(buffer.size());
        if (buffer_result.error_code != rpc::error::OK())
        {
            CO_RETURN transfer_result{buffer_result.error_code, 0, 0, 0};
        }

        struct context
        {
            uint32_t descriptor;
            std::shared_ptr<staging_buffer> buffer;
            uint32_t msg_flags;
        } operation_context{descriptor, std::move(buffer_result.buffer), msg_flags};

        // Staged send:
        //
        //     caller bytes ----memcpy----> staging slot ----SQE.addr----> kernel
        //
        // The staging_buffer shared_ptr in operation_context keeps the slot
        // alive until submit_operation has observed the send CQE.
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
                sqe.msg_flags = operation_context.msg_flags;
            },
            &operation_context);

        CO_RETURN transfer_result{result.error_code,
            result.native_result > 0 ? static_cast<uint32_t>(result.native_result) : 0U,
            result.native_result,
            result.cqe_flags};
    }

    // Receives bytes into the caller's span. Direct submission can use the
    // caller pointer directly when enabled; otherwise it receives into a
    // staging buffer first, then copies completed bytes back.
    CORO_TASK(transfer_result)
    controller::receive(
        uint32_t descriptor,
        rpc::mutable_byte_span buffer)
    {
        CO_RETURN CO_AWAIT receive_with_flags(descriptor, buffer, options_.receive_message_flags);
    }

    CORO_TASK(transfer_result)
    controller::receive_nonblocking(
        uint32_t descriptor,
        rpc::mutable_byte_span buffer)
    {
        CO_RETURN CO_AWAIT receive_with_flags(descriptor, buffer, options_.receive_message_flags | socket_message_dontwait);
    }

    CORO_TASK(transfer_result)
    controller::receive_with_flags(
        uint32_t descriptor,
        rpc::mutable_byte_span buffer,
        uint32_t msg_flags)
    {
        if (buffer.empty())
        {
            CO_RETURN transfer_result{rpc::error::OK(), 0, 0, 0};
        }

        auto err = CO_AWAIT ensure_iouring_data();
        if (err != rpc::error::OK())
        {
            CO_RETURN transfer_result{err, 0, 0, 0};
        }
        if (!cached_fixed_file_table_available())
        {
            if (!handle_)
            {
                CO_RETURN transfer_result{rpc::error::PROTOCOL_ERROR(), 0, 0, 0};
            }

            const bool nonblocking_receive = (msg_flags & socket_message_dontwait) != 0;
            while (can_accept_work())
            {
                auto result = CO_AWAIT handle_->receive(descriptor, buffer, msg_flags);
                if (result.error_code == rpc::error::OK() || nonblocking_receive
                    || !is_retryable_socket_result(result.native_result))
                {
                    CO_RETURN result;
                }

                CO_AWAIT wait_before_next_poll();
            }

            CO_RETURN transfer_result{shutdown_error(), 0, 0, 0};
        }

        if (options_.use_caller_buffers_for_transfers)
        {
            // Fast path:
            //
            //     kernel ----SQE.addr----> caller buffer
            const auto transfer_size = clamped_transfer_size(buffer.size());
            auto transfer_buffer = buffer.subspan(0, transfer_size);

            struct context
            {
                uint32_t descriptor;
                rpc::mutable_byte_span buffer;
                uint32_t msg_flags;
            } operation_context{descriptor, transfer_buffer, msg_flags};

            auto result = CO_AWAIT submit_operation(
                [](detail::sqe_64& sqe, void* data)
                {
                    auto& operation_context = *static_cast<context*>(data);
                    sqe.opcode = detail::io_uring_op_recv;
                    sqe.flags = detail::io_uring_sqe_fixed_file;
                    sqe.fd = static_cast<int32_t>(operation_context.descriptor);
                    sqe.addr = user_pointer_value(operation_context.buffer.data());
                    sqe.len = static_cast<uint32_t>(operation_context.buffer.size());
                    sqe.msg_flags = operation_context.msg_flags;
                },
                &operation_context);

            CO_RETURN transfer_result{result.error_code,
                result.native_result > 0 ? static_cast<uint32_t>(result.native_result) : 0U,
                result.native_result,
                result.cqe_flags};
        }

        auto buffer_result = CO_AWAIT allocate_staging_buffer(buffer.size());
        if (buffer_result.error_code != rpc::error::OK())
        {
            CO_RETURN transfer_result{buffer_result.error_code, 0, 0, 0};
        }

        struct context
        {
            uint32_t descriptor;
            std::shared_ptr<staging_buffer> buffer;
            uint32_t msg_flags;
        } operation_context{descriptor, std::move(buffer_result.buffer), msg_flags};

        // Staged receive:
        //
        //     kernel ----SQE.addr----> staging slot ----memcpy----> caller bytes
        //
        // Copy-back happens only after the CQE reports a successful positive
        // byte count.
        auto result = CO_AWAIT submit_operation(
            [](detail::sqe_64& sqe, void* data)
            {
                auto& operation_context = *static_cast<context*>(data);
                sqe.opcode = detail::io_uring_op_recv;
                sqe.flags = detail::io_uring_sqe_fixed_file;
                sqe.fd = static_cast<int32_t>(operation_context.descriptor);
                sqe.addr = operation_context.buffer->address();
                sqe.len = static_cast<uint32_t>(operation_context.buffer->size());
                sqe.msg_flags = operation_context.msg_flags;
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

    // Receives with a kernel-enforced timeout. Staged controllers place both
    // the receive target and timeout structure in staging buffers.
    // The RECV SQE is linked to IORING_OP_LINK_TIMEOUT so the kernel cancels
    // the receive if no bytes arrive before the deadline. The timeout structure
    // also lives in a staging buffer so it remains stable until completion.
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

        auto err = CO_AWAIT ensure_iouring_data();
        if (err != rpc::error::OK())
        {
            CO_RETURN transfer_result{err, 0, 0, 0};
        }
        if (!cached_fixed_file_table_available())
        {
            if (!handle_)
            {
                CO_RETURN transfer_result{rpc::error::PROTOCOL_ERROR(), 0, 0, 0};
            }

            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (can_accept_work())
            {
                auto result = CO_AWAIT handle_->receive(
                    descriptor, buffer, options_.receive_message_flags | socket_message_dontwait);
                if (result.error_code == rpc::error::OK())
                {
                    CO_RETURN result;
                }

                if (!is_retryable_socket_result(result.native_result))
                {
                    CO_RETURN result;
                }

                if (std::chrono::steady_clock::now() >= deadline)
                {
                    CO_RETURN transfer_result{rpc::error::CALL_TIMEOUT(), 0, -native_timer_expired, 0};
                }

                CO_AWAIT wait_before_next_poll();
            }

            CO_RETURN transfer_result{shutdown_error(), 0, 0, 0};
        }

        if (options_.use_caller_buffers_for_transfers)
        {
            const auto transfer_size = clamped_transfer_size(buffer.size());
            auto transfer_buffer = buffer.subspan(0, transfer_size);
            std::shared_ptr<detail::kernel_timespec> timeout_spec;
            try
            {
                timeout_spec = std::make_shared<detail::kernel_timespec>(make_kernel_timespec(timeout));
            }
            catch (const std::bad_alloc&)
            {
                RPC_ERROR("bad_alloc while creating direct io_uring receive timeout");
                std::terminate();
            }

            struct context
            {
                uint32_t descriptor;
                rpc::mutable_byte_span buffer;
                std::shared_ptr<detail::kernel_timespec> timeout_spec;
            } operation_context{descriptor, transfer_buffer, timeout_spec};

            auto result = CO_AWAIT submit_linked_operation(
                [](detail::sqe_64& recv_sqe, detail::sqe_64& timeout_sqe, void* data)
                {
                    auto& operation_context = *static_cast<context*>(data);
                    recv_sqe.opcode = detail::io_uring_op_recv;
                    recv_sqe.flags = detail::io_uring_sqe_fixed_file | detail::io_uring_sqe_io_link;
                    recv_sqe.fd = static_cast<int32_t>(operation_context.descriptor);
                    recv_sqe.addr = user_pointer_value(operation_context.buffer.data());
                    recv_sqe.len = static_cast<uint32_t>(operation_context.buffer.size());
                    recv_sqe.msg_flags = 0;

                    timeout_sqe.opcode = detail::io_uring_op_link_timeout;
                    timeout_sqe.addr = user_pointer_value(operation_context.timeout_spec.get());
                    timeout_sqe.len = 1;
                },
                &operation_context,
                timeout_spec);

            if (result.error_code != rpc::error::OK() && is_linked_timeout_result(result.native_result))
            {
                CO_RETURN transfer_result{rpc::error::CALL_TIMEOUT(), 0, result.native_result, result.cqe_flags};
            }

            CO_RETURN transfer_result{result.error_code,
                result.native_result > 0 ? static_cast<uint32_t>(result.native_result) : 0U,
                result.native_result,
                result.cqe_flags};
        }

        auto buffer_pair_result = CO_AWAIT allocate_staging_buffer_pair(buffer.size(), sizeof(detail::kernel_timespec));
        if (buffer_pair_result.error_code != rpc::error::OK())
        {
            CO_RETURN transfer_result{buffer_pair_result.error_code, 0, 0, 0};
        }

        // Timed receive uses two SQEs and two kernel-visible pointers:
        //
        //     RECV SQE.addr  --------> staging data buffer
        //     TIMEOUT SQE.addr -----> staging timespec buffer
        //
        // The pair allocation reserves both slots together. That prevents a
        // tiny pool from deadlocking with one coroutine holding a data buffer
        // while another holds the only timeout buffer.
        auto timeout_spec = make_kernel_timespec(timeout);
        std::memcpy(buffer_pair_result.second_buffer->data(), &timeout_spec, sizeof(timeout_spec));

        struct context
        {
            uint32_t descriptor;
            std::shared_ptr<staging_buffer> buffer;
            std::shared_ptr<staging_buffer> timeout_buffer;
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

        auto err = CO_AWAIT ensure_iouring_data();
        if (err != rpc::error::OK())
        {
            CO_RETURN operation_result{err, 0, 0};
        }
        if (!cached_fixed_file_table_available())
        {
            if (handle_)
            {
                CO_RETURN CO_AWAIT handle_->cancel_direct(descriptor);
            }
            CO_RETURN operation_result{rpc::error::PROTOCOL_ERROR(), 0, 0};
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

        auto err = CO_AWAIT ensure_iouring_data();
        if (err != rpc::error::OK())
        {
            CO_RETURN operation_result{err, 0, 0};
        }
        if (!cached_fixed_file_table_available())
        {
            if (handle_)
            {
                CO_RETURN CO_AWAIT handle_->close_direct(descriptor);
            }
            CO_RETURN operation_result{rpc::error::PROTOCOL_ERROR(), 0, 0};
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
