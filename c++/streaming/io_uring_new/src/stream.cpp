/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <streaming/io_uring_new/stream.h>

#include <io_uring/controller.h>

#include <cerrno>
#include <new>
#include <utility>

namespace streaming::io_uring_new
{
    namespace
    {
        static inline coro::net::io_status ok_status() noexcept
        {
            return coro::net::io_status{.type = coro::net::io_status::kind::ok};
        }

        static inline coro::net::io_status closed_status() noexcept
        {
            return coro::net::io_status{.type = coro::net::io_status::kind::closed};
        }

        static inline coro::net::io_status timeout_status() noexcept
        {
            return coro::net::io_status{.type = coro::net::io_status::kind::timeout};
        }

        static inline int32_t native_error_code(int32_t native_result) noexcept
        {
            return native_result < 0 ? -native_result : EIO;
        }

        static inline coro::net::io_status native_status(int32_t native_result) noexcept
        {
            return coro::net::io_status{
                .type = coro::net::io_status::kind::native, .native_code = native_error_code(native_result)};
        }

        static inline bool is_retryable_native_result(int32_t native_result) noexcept
        {
            const auto native_code = native_error_code(native_result);
            return native_code == EAGAIN || native_code == EWOULDBLOCK || native_code == EINTR;
        }
    } // namespace

    stream::stream(
        std::shared_ptr<rpc::io_uring::direct_descriptor> descriptor,
        uint16_t peer_port) noexcept
        : descriptor_(std::move(descriptor))
        , peer_port_(peer_port)
    {
    }

    auto stream::receive(
        rpc::mutable_byte_span buffer,
        std::chrono::milliseconds timeout)
        -> coro::task<std::pair<
            coro::net::io_status,
            rpc::mutable_byte_span>>
    {
        auto descriptor = descriptor_;
        if (closed_.load(std::memory_order_acquire) || !descriptor || !descriptor->is_open())
        {
            co_return std::pair{closed_status(), rpc::mutable_byte_span{}};
        }

        auto deadline = std::chrono::steady_clock::time_point{};
        const bool use_deadline = timeout > std::chrono::milliseconds{0};
        if (use_deadline)
        {
            deadline = std::chrono::steady_clock::now() + timeout;
        }

        while (true)
        {
            auto controller = descriptor->get_controller();
            auto descriptor_index = descriptor->get();
            if (!controller || descriptor_index == rpc::io_uring::direct_descriptor::invalid_descriptor)
            {
                closed_.store(true, std::memory_order_release);
                co_return std::pair{closed_status(), rpc::mutable_byte_span{}};
            }

            auto operation_timeout = timeout;
            if (use_deadline)
            {
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline)
                {
                    co_return std::pair{timeout_status(), rpc::mutable_byte_span{}};
                }

                operation_timeout = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
                if (operation_timeout <= std::chrono::milliseconds{0})
                {
                    operation_timeout = std::chrono::milliseconds{1};
                }
            }

            auto result = co_await controller->receive(descriptor_index, buffer, operation_timeout);
            if (result.error_code == rpc::error::CALL_TIMEOUT())
            {
                if (closed_.load(std::memory_order_acquire) || !descriptor->is_open())
                {
                    co_return std::pair{closed_status(), rpc::mutable_byte_span{}};
                }

                co_return std::pair{timeout_status(), rpc::mutable_byte_span{}};
            }

            if (result.error_code != rpc::error::OK())
            {
                if (is_retryable_native_result(result.native_result))
                {
                    continue;
                }

                closed_.store(true, std::memory_order_release);
                co_await descriptor->close();
                co_return std::pair{native_status(result.native_result), rpc::mutable_byte_span{}};
            }

            if (result.native_result == 0)
            {
                closed_.store(true, std::memory_order_release);
                co_await descriptor->close();
                co_return std::pair{closed_status(), rpc::mutable_byte_span{}};
            }

            co_return std::pair{ok_status(), buffer.subspan(0, result.bytes_transferred)};
        }
    }

    auto stream::send(rpc::byte_span buffer) -> coro::task<coro::net::io_status>
    {
        auto descriptor = descriptor_;
        if (closed_.load(std::memory_order_acquire) || !descriptor || !descriptor->is_open())
        {
            co_return closed_status();
        }

        while (!buffer.empty())
        {
            auto controller = descriptor->get_controller();
            auto descriptor_index = descriptor->get();
            if (!controller || descriptor_index == rpc::io_uring::direct_descriptor::invalid_descriptor)
            {
                closed_.store(true, std::memory_order_release);
                co_return closed_status();
            }

            auto result = co_await controller->send(descriptor_index, buffer);
            if (result.error_code != rpc::error::OK())
            {
                if (is_retryable_native_result(result.native_result))
                {
                    continue;
                }

                closed_.store(true, std::memory_order_release);
                co_await descriptor->close();
                co_return native_status(result.native_result);
            }

            if (result.bytes_transferred == 0)
            {
                closed_.store(true, std::memory_order_release);
                co_await descriptor->close();
                co_return closed_status();
            }

            buffer = buffer.subspan(result.bytes_transferred);
        }

        co_return ok_status();
    }

    bool stream::is_closed() const
    {
        auto descriptor = descriptor_;
        return closed_.load(std::memory_order_acquire) || !descriptor || !descriptor->is_open();
    }

    auto stream::set_closed() -> coro::task<void>
    {
        auto descriptor = descriptor_;
        if (!closed_.exchange(true, std::memory_order_acq_rel) && descriptor)
        {
            co_await descriptor->close();
        }
        co_return;
    }

    streaming::peer_info stream::get_peer_info() const
    {
        streaming::peer_info info{};
        info.port = peer_port_;
#ifndef FOR_SGX
        info.family = canopy::network_config::ip_address_family::ipv4;
        info.addr[0] = 127;
        info.addr[1] = 0;
        info.addr[2] = 0;
        info.addr[3] = 1;
#endif
        return info;
    }

    std::shared_ptr<streaming::stream> make_stream(
        std::shared_ptr<rpc::io_uring::direct_descriptor> descriptor,
        uint16_t peer_port) noexcept
    {
        if (!descriptor)
        {
            return {};
        }

        try
        {
            return std::make_shared<stream>(std::move(descriptor), peer_port);
        }
        catch (...)
        {
            return {};
        }
    }

    stream_result make_stream_result(
        const rpc::io_uring::direct_descriptor_result& result,
        uint16_t peer_port) noexcept
    {
        if (result.error_code != rpc::error::OK() || !result.descriptor)
        {
            return stream_result{result.error_code, result.native_result, result.cqe_flags, {}};
        }

        auto connection = make_stream(result.descriptor, peer_port);
        if (!connection)
        {
            return stream_result{rpc::error::OUT_OF_MEMORY(), result.native_result, result.cqe_flags, {}};
        }

        return stream_result{rpc::error::OK(), result.native_result, result.cqe_flags, std::move(connection)};
    }
} // namespace streaming::io_uring_new
