/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <file_system/file_system_manager.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <utility>
#include <vector>

namespace rpc
{
    namespace file_system
    {
        inline namespace v1
        {
            namespace
            {
                static constexpr uint32_t open_flag_read_only = 0;
                static constexpr uint32_t open_flag_write_only = 1;
                static constexpr uint32_t open_flag_create = 0100;
                static constexpr uint32_t open_flag_truncate = 01000;
                static constexpr uint32_t open_mode_user_read_write = 0600;
                static constexpr size_t default_transfer_size = 64U * 1024U;
            }

            class manager_impl final : public rpc::base<manager_impl, i_manager>
            {
            public:
                explicit manager_impl(std::shared_ptr<rpc::io_uring::controller> controller) noexcept
                    : controller_(std::move(controller))
                {
                }

                CORO_TASK(int)
                list_files(
                    std::string directory,
                    std::vector<file_info>& files) override
                {
                    (void)directory;
                    files.clear();
                    // The current Linux/io_uring ABI in this tree has no
                    // getdents/readdir SQE. Do not hide a syscall or host shim
                    // behind an enclave-facing method that must stay direct.
                    CO_RETURN rpc::error::NOT_IMPLEMENTED();
                }

                CORO_TASK(int)
                read_file(
                    std::string file_name,
                    std::vector<uint8_t>& data) override
                {
                    data.clear();
                    if (!controller_ || file_name.empty())
                    {
                        CO_RETURN rpc::error::INVALID_DATA();
                    }

                    auto open_result = CO_AWAIT controller_->open_file(std::move(file_name), open_flag_read_only, 0);
                    if (open_result.error_code != rpc::error::OK())
                    {
                        CO_RETURN open_result.error_code;
                    }

                    int error_code = rpc::error::OK();
                    uint64_t offset = 0;
                    std::vector<uint8_t> buffer;
                    try
                    {
                        buffer.resize(default_transfer_size);
                    }
                    catch (const std::bad_alloc&)
                    {
                        error_code = rpc::error::OUT_OF_MEMORY();
                    }

                    while (error_code == rpc::error::OK())
                    {
                        auto read_result = CO_AWAIT controller_->read_at(
                            open_result.descriptor, rpc::mutable_byte_span(buffer), offset);
                        if (read_result.error_code != rpc::error::OK())
                        {
                            error_code = read_result.error_code;
                            break;
                        }

                        if (read_result.bytes_transferred == 0)
                        {
                            break;
                        }

                        if (offset > std::numeric_limits<uint64_t>::max() - read_result.bytes_transferred)
                        {
                            error_code = rpc::error::INVALID_DATA();
                            break;
                        }

                        try
                        {
                            data.insert(
                                data.end(),
                                buffer.begin(),
                                buffer.begin() + static_cast<std::ptrdiff_t>(read_result.bytes_transferred));
                        }
                        catch (const std::bad_alloc&)
                        {
                            error_code = rpc::error::OUT_OF_MEMORY();
                            break;
                        }

                        offset += read_result.bytes_transferred;
                    }

                    auto close_result = CO_AWAIT controller_->close_direct(open_result.descriptor);
                    if (error_code != rpc::error::OK())
                    {
                        data.clear();
                        CO_RETURN error_code;
                    }

                    CO_RETURN close_result.error_code;
                }

                CORO_TASK(int)
                write_file(
                    std::string file_name,
                    const std::vector<uint8_t>& data) override
                {
                    if (!controller_ || file_name.empty())
                    {
                        CO_RETURN rpc::error::INVALID_DATA();
                    }

                    auto open_result = CO_AWAIT controller_->open_file(
                        std::move(file_name),
                        open_flag_write_only | open_flag_create | open_flag_truncate,
                        open_mode_user_read_write);
                    if (open_result.error_code != rpc::error::OK())
                    {
                        CO_RETURN open_result.error_code;
                    }

                    int error_code = rpc::error::OK();
                    size_t bytes_written = 0;
                    while (bytes_written < data.size())
                    {
                        const auto remaining = data.size() - bytes_written;
                        const auto chunk_size = remaining < default_transfer_size ? remaining : default_transfer_size;
                        auto write_result = CO_AWAIT controller_->write_at(
                            open_result.descriptor,
                            rpc::byte_span(data.data() + bytes_written, chunk_size),
                            static_cast<uint64_t>(bytes_written));

                        if (write_result.error_code != rpc::error::OK())
                        {
                            error_code = write_result.error_code;
                            break;
                        }
                        if (write_result.bytes_transferred == 0)
                        {
                            error_code = rpc::error::NATIVE_IO_ERROR();
                            break;
                        }
                        if (write_result.bytes_transferred > remaining)
                        {
                            error_code = rpc::error::PROTOCOL_ERROR();
                            break;
                        }

                        bytes_written += write_result.bytes_transferred;
                    }

                    auto close_result = CO_AWAIT controller_->close_direct(open_result.descriptor);
                    CO_RETURN error_code == rpc::error::OK() ? close_result.error_code : error_code;
                }

            private:
                std::shared_ptr<rpc::io_uring::controller> controller_;
            };

            rpc::shared_ptr<i_manager> create_factory(std::shared_ptr<rpc::io_uring::controller> controller)
            {
                return rpc::make_shared<manager_impl>(std::move(controller));
            }
        }
    }
}
