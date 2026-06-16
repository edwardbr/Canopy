/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <io_uring/controller.h>

#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>

namespace rpc::io_uring
{
    namespace
    {
        static constexpr int32_t at_fdcwd = -100;

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

    // File I/O uses the same direct-ring staging rule as TCP:
    //
    //     open path:    C string      -> staging slot -> SQE.addr
    //     read_at:      SQE.addr      -> staging slot -> caller bytes
    //     write_at:     caller bytes  -> staging slot -> SQE.addr
    //
    // Host builds may submit caller buffers directly when configured to do so;
    // otherwise transfers stage through the configured buffer pool.

    // Opens a path directly into the io_uring fixed-file table. The returned
    // descriptor is a direct descriptor index, not a process file descriptor.
    CORO_TASK(descriptor_result)
    controller::open_file(
        std::string path,
        uint32_t open_flags,
        uint32_t mode)
    {
        if (path.empty() || path.size() == std::numeric_limits<size_t>::max())
        {
            CO_RETURN descriptor_result{rpc::error::INVALID_DATA(), 0, 0, 0};
        }

        auto err = CO_AWAIT ensure_iouring_data();
        if (err != rpc::error::OK())
        {
            CO_RETURN descriptor_result{err, 0, 0, 0};
        }
        if (!cached_fixed_file_table_available())
        {
            if (handle_)
            {
                CO_RETURN CO_AWAIT handle_->open_file(std::move(path), open_flags, mode);
            }
            CO_RETURN descriptor_result{rpc::error::PROTOCOL_ERROR(), 0, 0, 0};
        }

        const auto path_size = path.size() + 1;
        auto path_buffer_result = CO_AWAIT allocate_staging_buffer(path_size);
        if (path_buffer_result.error_code != rpc::error::OK())
        {
            CO_RETURN descriptor_result{path_buffer_result.error_code, 0, 0, 0};
        }
        if (!path_buffer_result.buffer || path_buffer_result.buffer->size() < path_size)
        {
            CO_RETURN descriptor_result{rpc::error::RESOURCE_EXHAUSTED(), 0, 0, 0};
        }

        // The kernel expects a NUL-terminated path at SQE.addr. Copy it into a
        // staging slot so the path remains stable until completion.
        std::memcpy(path_buffer_result.buffer->data(), path.data(), path.size());
        path_buffer_result.buffer->data()[path.size()] = 0;

        struct context
        {
            std::shared_ptr<staging_buffer> path_buffer;
            uint32_t open_flags;
            uint32_t mode;
        } operation_context{std::move(path_buffer_result.buffer), open_flags, mode};

        auto result = CO_AWAIT submit_operation(
            [](detail::sqe_64& sqe, void* data)
            {
                auto& operation_context = *static_cast<context*>(data);
                sqe.opcode = detail::io_uring_op_openat;
                sqe.fd = at_fdcwd;
                sqe.addr = operation_context.path_buffer->address();
                sqe.len = operation_context.mode;
                sqe.open_flags = operation_context.open_flags;
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

    CORO_TASK(transfer_result)
    controller::read_at(
        uint32_t descriptor,
        rpc::mutable_byte_span buffer,
        uint64_t offset)
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
            if (handle_)
            {
                CO_RETURN CO_AWAIT handle_->read_at(descriptor, buffer, offset);
            }
            CO_RETURN transfer_result{rpc::error::PROTOCOL_ERROR(), 0, 0, 0};
        }

        if (options_.use_caller_buffers_for_transfers)
        {
            // Fast path: the kernel writes directly into the caller's span.
            const auto transfer_size = clamped_transfer_size(buffer.size());
            auto transfer_buffer = buffer.subspan(0, transfer_size);

            struct context
            {
                uint32_t descriptor;
                rpc::mutable_byte_span buffer;
                uint64_t offset;
            } operation_context{descriptor, transfer_buffer, offset};

            auto result = CO_AWAIT submit_operation(
                [](detail::sqe_64& sqe, void* data)
                {
                    auto& operation_context = *static_cast<context*>(data);
                    sqe.opcode = detail::io_uring_op_read;
                    sqe.flags = detail::io_uring_sqe_fixed_file;
                    sqe.fd = static_cast<int32_t>(operation_context.descriptor);
                    sqe.addr = user_pointer_value(operation_context.buffer.data());
                    sqe.len = static_cast<uint32_t>(operation_context.buffer.size());
                    sqe.off = operation_context.offset;
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
            uint64_t offset;
        } operation_context{descriptor, std::move(buffer_result.buffer), offset};

        // Staged file read:
        //
        //     kernel ----SQE.addr----> staging slot ----memcpy----> caller span
        //
        // Copy-back happens after a successful CQE with a positive byte count.
        auto result = CO_AWAIT submit_operation(
            [](detail::sqe_64& sqe, void* data)
            {
                auto& operation_context = *static_cast<context*>(data);
                sqe.opcode = detail::io_uring_op_read;
                sqe.flags = detail::io_uring_sqe_fixed_file;
                sqe.fd = static_cast<int32_t>(operation_context.descriptor);
                sqe.addr = operation_context.buffer->address();
                sqe.len = static_cast<uint32_t>(operation_context.buffer->size());
                sqe.off = operation_context.offset;
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

    CORO_TASK(transfer_result)
    controller::write_at(
        uint32_t descriptor,
        rpc::byte_span buffer,
        uint64_t offset)
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
            if (handle_)
            {
                CO_RETURN CO_AWAIT handle_->write_at(descriptor, buffer, offset);
            }
            CO_RETURN transfer_result{rpc::error::PROTOCOL_ERROR(), 0, 0, 0};
        }

        if (options_.use_caller_buffers_for_transfers)
        {
            // Fast path: the kernel reads directly from the caller's span.
            const auto transfer_size = clamped_transfer_size(buffer.size());
            auto transfer_buffer = buffer.subspan(0, transfer_size);

            struct context
            {
                uint32_t descriptor;
                rpc::byte_span buffer;
                uint64_t offset;
            } operation_context{descriptor, transfer_buffer, offset};

            auto result = CO_AWAIT submit_operation(
                [](detail::sqe_64& sqe, void* data)
                {
                    auto& operation_context = *static_cast<context*>(data);
                    sqe.opcode = detail::io_uring_op_write;
                    sqe.flags = detail::io_uring_sqe_fixed_file;
                    sqe.fd = static_cast<int32_t>(operation_context.descriptor);
                    sqe.addr = user_pointer_value(operation_context.buffer.data());
                    sqe.len = static_cast<uint32_t>(operation_context.buffer.size());
                    sqe.off = operation_context.offset;
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
            uint64_t offset;
        } operation_context{descriptor, std::move(buffer_result.buffer), offset};

        // Staged file write:
        //
        //     caller span ----memcpy----> staging slot ----SQE.addr----> kernel
        //
        // The staging slot is retained by operation_context until the CQE is
        // consumed, so the kernel never observes a dangling buffer pointer.
        std::memcpy(operation_context.buffer->data(), buffer.data(), operation_context.buffer->size());
        auto result = CO_AWAIT submit_operation(
            [](detail::sqe_64& sqe, void* data)
            {
                auto& operation_context = *static_cast<context*>(data);
                sqe.opcode = detail::io_uring_op_write;
                sqe.flags = detail::io_uring_sqe_fixed_file;
                sqe.fd = static_cast<int32_t>(operation_context.descriptor);
                sqe.addr = operation_context.buffer->address();
                sqe.len = static_cast<uint32_t>(operation_context.buffer->size());
                sqe.off = operation_context.offset;
            },
            &operation_context);

        CO_RETURN transfer_result{result.error_code,
            result.native_result > 0 ? static_cast<uint32_t>(result.native_result) : 0U,
            result.native_result,
            result.cqe_flags};
    }
} // namespace rpc::io_uring
