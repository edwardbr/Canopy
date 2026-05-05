/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <memory>
#include <new>
#include <utility>

#include <io_uring/controller.h>
#include <io_uring/direct_descriptor.h>
#include <streaming/stream.h>

namespace rpc::io_uring
{
    namespace detail
    {
        static inline ::coro::net::io_status ok_status() noexcept
        {
            return ::coro::net::io_status{.type = ::coro::net::io_status::kind::ok};
        }

        static inline ::coro::net::io_status closed_status() noexcept
        {
            return ::coro::net::io_status{.type = ::coro::net::io_status::kind::closed};
        }

        static inline ::coro::net::io_status timeout_status() noexcept
        {
            return ::coro::net::io_status{.type = ::coro::net::io_status::kind::timeout};
        }

        static inline int32_t native_error_code(int32_t native_result) noexcept
        {
            return native_result < 0 ? -native_result : EIO;
        }

        static inline ::coro::net::io_status native_status(int32_t native_result) noexcept
        {
            return ::coro::net::io_status{
                .type = ::coro::net::io_status::kind::native, .native_code = native_error_code(native_result)};
        }

        static inline bool is_retryable_native_result(int32_t native_result) noexcept
        {
            const auto native_code = native_error_code(native_result);
            return native_code == EAGAIN || native_code == EWOULDBLOCK || native_code == EINTR;
        }
    } // namespace detail

    class stream : public streaming::stream
    {
    public:
        stream(
            std::shared_ptr<direct_descriptor> descriptor,
            uint16_t peer_port = 0) noexcept
            : descriptor_(std::move(descriptor))
            , peer_port_(peer_port)
        {
        }

        auto receive(
            rpc::mutable_byte_span buffer,
            std::chrono::milliseconds timeout = std::chrono::milliseconds{0})
            -> ::coro::task<std::pair<
                ::coro::net::io_status,
                rpc::mutable_byte_span>> override
        {
            auto descriptor = descriptor_;
            if (closed_.load(std::memory_order_acquire) || !descriptor || !descriptor->is_open())
            {
                co_return std::pair{detail::closed_status(), rpc::mutable_byte_span{}};
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
                if (!controller || descriptor_index == direct_descriptor::invalid_descriptor)
                {
                    closed_.store(true, std::memory_order_release);
                    co_return std::pair{detail::closed_status(), rpc::mutable_byte_span{}};
                }

                auto operation_timeout = timeout;
                if (use_deadline)
                {
                    const auto now = std::chrono::steady_clock::now();
                    if (now >= deadline)
                    {
                        co_return std::pair{detail::timeout_status(), rpc::mutable_byte_span{}};
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
                        co_return std::pair{detail::closed_status(), rpc::mutable_byte_span{}};
                    }

                    co_return std::pair{detail::timeout_status(), rpc::mutable_byte_span{}};
                }

                if (result.error_code != rpc::error::OK())
                {
                    if (detail::is_retryable_native_result(result.native_result))
                    {
                        continue;
                    }

                    closed_.store(true, std::memory_order_release);
                    co_await descriptor->close();
                    co_return std::pair{detail::native_status(result.native_result), rpc::mutable_byte_span{}};
                }

                if (result.native_result == 0)
                {
                    closed_.store(true, std::memory_order_release);
                    co_await descriptor->close();
                    co_return std::pair{detail::closed_status(), rpc::mutable_byte_span{}};
                }

                co_return std::pair{detail::ok_status(), buffer.subspan(0, result.bytes_transferred)};
            }
        }

        auto send(rpc::byte_span buffer) -> ::coro::task<::coro::net::io_status> override
        {
            auto descriptor = descriptor_;
            if (closed_.load(std::memory_order_acquire) || !descriptor || !descriptor->is_open())
            {
                co_return detail::closed_status();
            }

            while (!buffer.empty())
            {
                auto controller = descriptor->get_controller();
                auto descriptor_index = descriptor->get();
                if (!controller || descriptor_index == direct_descriptor::invalid_descriptor)
                {
                    closed_.store(true, std::memory_order_release);
                    co_return detail::closed_status();
                }

                auto result = co_await controller->send(descriptor_index, buffer);
                if (result.error_code != rpc::error::OK())
                {
                    if (detail::is_retryable_native_result(result.native_result))
                    {
                        continue;
                    }

                    closed_.store(true, std::memory_order_release);
                    co_await descriptor->close();
                    co_return detail::native_status(result.native_result);
                }

                if (result.bytes_transferred == 0)
                {
                    closed_.store(true, std::memory_order_release);
                    co_await descriptor->close();
                    co_return detail::closed_status();
                }

                buffer = buffer.subspan(result.bytes_transferred);
            }

            co_return detail::ok_status();
        }

        [[nodiscard]] bool is_closed() const override
        {
            auto descriptor = descriptor_;
            return closed_.load(std::memory_order_acquire) || !descriptor || !descriptor->is_open();
        }

        auto set_closed() -> ::coro::task<void> override
        {
            auto descriptor = descriptor_;
            if (!closed_.exchange(true, std::memory_order_acq_rel) && descriptor)
            {
                co_await descriptor->close();
            }
            co_return;
        }

        [[nodiscard]] streaming::peer_info get_peer_info() const override
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

    private:
        std::shared_ptr<direct_descriptor> descriptor_;
        uint16_t peer_port_{0};
        std::atomic<bool> closed_{false};
    };

    struct stream_result
    {
        int error_code{rpc::error::OK()};
        int32_t native_result{0};
        uint32_t cqe_flags{0};
        std::shared_ptr<streaming::stream> connection;
    };

    class acceptor
    {
    public:
        explicit acceptor(std::shared_ptr<controller> controller) noexcept
            : controller_(std::move(controller))
        {
        }

        CORO_TASK(int)
        listen_loopback(
            uint16_t port,
            uint32_t backlog = 16)
        {
            if (!controller_)
            {
                CO_RETURN rpc::error::RESOURCE_CLOSED();
            }

            if (listen_descriptor_)
            {
                CO_AWAIT close();
            }

            auto socket_result = CO_AWAIT controller_->create_tcp_socket();
            if (socket_result.error_code != rpc::error::OK())
            {
                CO_RETURN socket_result.error_code;
            }

            auto allocation_error = rpc::error::OK();
            try
            {
                listen_descriptor_ = std::make_shared<direct_descriptor>(controller_, socket_result.descriptor);
            }
            catch (const std::bad_alloc&)
            {
                allocation_error = rpc::error::OUT_OF_MEMORY();
            }
            catch (...)
            {
                allocation_error = rpc::error::EXCEPTION();
            }

            if (allocation_error != rpc::error::OK() || !listen_descriptor_)
            {
                CO_AWAIT controller_->close_direct(socket_result.descriptor);
                CO_RETURN allocation_error != rpc::error::OK() ? allocation_error : rpc::error::OUT_OF_MEMORY();
            }

            port_ = port;
            auto bind_result = CO_AWAIT controller_->bind_tcp_ipv4_loopback(listen_descriptor_->get(), port);
            if (bind_result.error_code != rpc::error::OK())
            {
                CO_AWAIT close();
                CO_RETURN bind_result.error_code;
            }

            auto listen_result = CO_AWAIT controller_->listen(listen_descriptor_->get(), backlog);
            if (listen_result.error_code != rpc::error::OK())
            {
                CO_AWAIT close();
                CO_RETURN listen_result.error_code;
            }

            listening_ = true;
            CO_RETURN rpc::error::OK();
        }

        CORO_TASK(stream_result) accept_with_result()
        {
            auto listen_descriptor = listen_descriptor_;
            if (!controller_ || !listening_ || !listen_descriptor || !listen_descriptor->is_open())
            {
                CO_RETURN stream_result{rpc::error::RESOURCE_CLOSED(), 0, 0, {}};
            }

            auto accept_result = CO_AWAIT controller_->accept(listen_descriptor->get());
            if (accept_result.error_code != rpc::error::OK())
            {
                CO_RETURN stream_result{accept_result.error_code, accept_result.native_result, accept_result.cqe_flags, {}};
            }

            std::shared_ptr<direct_descriptor> accepted_descriptor;
            auto allocation_error = rpc::error::OK();
            try
            {
                accepted_descriptor = std::make_shared<direct_descriptor>(controller_, accept_result.descriptor);
            }
            catch (const std::bad_alloc&)
            {
                allocation_error = rpc::error::OUT_OF_MEMORY();
            }
            catch (...)
            {
                allocation_error = rpc::error::EXCEPTION();
            }

            if (allocation_error != rpc::error::OK() || !accepted_descriptor)
            {
                CO_AWAIT controller_->close_direct(accept_result.descriptor);
                CO_RETURN stream_result{
                    allocation_error != rpc::error::OK() ? allocation_error : rpc::error::OUT_OF_MEMORY(),
                    accept_result.native_result,
                    accept_result.cqe_flags,
                    {}};
            }

            std::shared_ptr<streaming::stream> created_stream;
            try
            {
                created_stream = std::make_shared<stream>(std::move(accepted_descriptor), port_);
            }
            catch (const std::bad_alloc&)
            {
                allocation_error = rpc::error::OUT_OF_MEMORY();
            }
            catch (...)
            {
                allocation_error = rpc::error::EXCEPTION();
            }

            if (allocation_error != rpc::error::OK() || !created_stream)
            {
                if (accepted_descriptor)
                {
                    CO_AWAIT accepted_descriptor->close();
                }
                CO_RETURN stream_result{
                    allocation_error != rpc::error::OK() ? allocation_error : rpc::error::OUT_OF_MEMORY(),
                    accept_result.native_result,
                    accept_result.cqe_flags,
                    {}};
            }

            CO_RETURN stream_result{
                rpc::error::OK(), accept_result.native_result, accept_result.cqe_flags, std::move(created_stream)};
        }

        CORO_TASK(std::shared_ptr<streaming::stream>) accept()
        {
            auto result = CO_AWAIT accept_with_result();
            CO_RETURN std::move(result.connection);
        }

        CORO_TASK(void) close()
        {
            auto listen_descriptor = std::move(listen_descriptor_);
            if (listen_descriptor)
            {
                CO_AWAIT listen_descriptor->close();
            }
            listening_ = false;
            CO_RETURN;
        }

        [[nodiscard]] uint16_t port() const noexcept { return port_; }
        [[nodiscard]] bool is_listening() const noexcept
        {
            auto listen_descriptor = listen_descriptor_;
            return listening_ && listen_descriptor && listen_descriptor->is_open();
        }

    private:
        std::shared_ptr<controller> controller_;
        std::shared_ptr<direct_descriptor> listen_descriptor_;
        uint16_t port_{0};
        bool listening_{false};
    };

    class connector
    {
    public:
        explicit connector(std::shared_ptr<controller> controller) noexcept
            : controller_(std::move(controller))
        {
        }

        CORO_TASK(stream_result) connect_loopback_with_result(uint16_t port)
        {
            if (!controller_)
            {
                CO_RETURN stream_result{rpc::error::RESOURCE_CLOSED(), 0, 0, {}};
            }

            auto connect_result = CO_AWAIT controller_->connect_tcp_ipv4_loopback(port);
            if (connect_result.error_code != rpc::error::OK())
            {
                CO_RETURN stream_result{
                    connect_result.error_code, connect_result.native_result, connect_result.cqe_flags, {}};
            }

            std::shared_ptr<direct_descriptor> descriptor;
            auto allocation_error = rpc::error::OK();
            try
            {
                descriptor = std::make_shared<direct_descriptor>(controller_, connect_result.descriptor);
            }
            catch (const std::bad_alloc&)
            {
                allocation_error = rpc::error::OUT_OF_MEMORY();
            }
            catch (...)
            {
                allocation_error = rpc::error::EXCEPTION();
            }

            if (allocation_error != rpc::error::OK() || !descriptor)
            {
                CO_AWAIT controller_->close_direct(connect_result.descriptor);
                CO_RETURN stream_result{
                    allocation_error != rpc::error::OK() ? allocation_error : rpc::error::OUT_OF_MEMORY(),
                    connect_result.native_result,
                    connect_result.cqe_flags,
                    {}};
            }

            std::shared_ptr<streaming::stream> created_stream;
            try
            {
                created_stream = std::make_shared<stream>(std::move(descriptor), port);
            }
            catch (const std::bad_alloc&)
            {
                allocation_error = rpc::error::OUT_OF_MEMORY();
            }
            catch (...)
            {
                allocation_error = rpc::error::EXCEPTION();
            }

            if (allocation_error != rpc::error::OK() || !created_stream)
            {
                if (descriptor)
                {
                    CO_AWAIT descriptor->close();
                }
                CO_RETURN stream_result{
                    allocation_error != rpc::error::OK() ? allocation_error : rpc::error::OUT_OF_MEMORY(),
                    connect_result.native_result,
                    connect_result.cqe_flags,
                    {}};
            }

            CO_RETURN stream_result{
                rpc::error::OK(), connect_result.native_result, connect_result.cqe_flags, std::move(created_stream)};
        }

        CORO_TASK(std::shared_ptr<streaming::stream>) connect_loopback(uint16_t port)
        {
            auto result = CO_AWAIT connect_loopback_with_result(port);
            CO_RETURN std::move(result.connection);
        }

    private:
        std::shared_ptr<controller> controller_;
    };

} // namespace rpc::io_uring
