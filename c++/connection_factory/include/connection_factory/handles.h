/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

#include <connection_factory/service.h>
#include <streaming/listener.h>
#include <streaming/stream.h>
#include <streaming/stream_acceptor.h>

namespace rpc::connection_factory
{
    // These handles are the ownership boundary returned by the simplified
    // factories. They hide transports/listeners from callers that only need a
    // stream or RPC endpoint, while still keeping the service, acceptor, and
    // optional runtime owner alive for as long as the handle is retained.

    // Keeps a scheduler/runtime owner alive for streams created by factories
    // that did not receive a caller-owned service. The wrapper is transparent
    // to stream users.
    class owning_stream final : public ::streaming::stream
    {
    public:
        owning_stream(
            std::shared_ptr<::streaming::stream> inner,
            std::shared_ptr<void> owner)
            : inner_(std::move(inner))
            , owner_(std::move(owner))
        {
        }

        auto receive(
            rpc::mutable_byte_span buffer,
            std::chrono::milliseconds timeout = std::chrono::milliseconds{0})
            -> CORO_TASK(::streaming::receive_result) override
        {
            CO_RETURN CO_AWAIT inner_->receive(buffer, timeout);
        }

        auto send(rpc::byte_span buffer) -> CORO_TASK(rpc::io_status) override
        {
            CO_RETURN CO_AWAIT inner_->send(buffer);
        }

        [[nodiscard]] bool is_closed() const override { return inner_->is_closed(); }

        auto set_closed() -> CORO_TASK(void) override
        {
            CO_AWAIT inner_->set_closed();
            CO_RETURN;
        }

        [[nodiscard]] ::streaming::peer_info get_peer_info() const override { return inner_->get_peer_info(); }

    private:
        std::shared_ptr<::streaming::stream> inner_;
        std::shared_ptr<void> owner_;
    };

    inline std::shared_ptr<::streaming::stream> keep_owner(
        std::shared_ptr<::streaming::stream> stream,
        std::shared_ptr<void> owner)
    {
        // Some base stream implementations are backed by a short-lived factory
        // owner, for example an io_uring controller. Wrapping the stream avoids
        // leaking that type into the public result while preserving its lifetime.
        if (!owner)
            return stream;
        return std::make_shared<owning_stream>(std::move(stream), std::move(owner));
    }

    // Stream-only factories return this lightweight result rather than exposing
    // transport/service internals.
    struct stream_result
    {
        int error_code{rpc::error::OK()};
        std::shared_ptr<::streaming::stream> stream;
        int32_t native_result{0};
    };

    using stream_callback = std::function<CORO_TASK(void)(std::shared_ptr<::streaming::stream>)>;

    // Owns the accept loop for stream-only acceptors. RPC listeners use
    // streaming::listener instead; this class is for raw stream callbacks.
    class stream_accept_handle : public std::enable_shared_from_this<stream_accept_handle>
    {
    public:
        stream_accept_handle(
            std::shared_ptr<::streaming::stream_acceptor> acceptor,
            std::shared_ptr<rpc::service> service,
            stream_callback callback,
            std::shared_ptr<void> owner = {},
            uint16_t port = 0)
            : acceptor_(std::move(acceptor))
            , service_(std::move(service))
            , executor_(service_ ? service_->get_executor() : nullptr)
            , callback_(std::move(callback))
            , owner_(std::move(owner))
            , port_(port)
        {
        }

        [[nodiscard]] bool start()
        {
            bool expected = false;
            if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
                return false;
            if (!acceptor_ || !executor_ || !acceptor_->init(executor_))
            {
                running_.store(false, std::memory_order_release);
                return false;
            }

            if (!executor_->SPAWN_DETACHED(run(shared_from_this())))
            {
                acceptor_->stop();
                running_.store(false, std::memory_order_release);
                return false;
            }
            return true;
        }

        CORO_TASK(void) stop()
        {
            if (!running_.load(std::memory_order_acquire))
            {
                service_.reset();
                CO_RETURN;
            }

            stopping_.store(true, std::memory_order_release);
            acceptor_->stop();
            CO_AWAIT stopped_.wait();
            service_.reset();
            CO_RETURN;
        }

        [[nodiscard]] uint16_t port() const noexcept { return port_; }

    private:
        CORO_TASK(void) run(std::shared_ptr<stream_accept_handle> self)
        {
            (void)self;
            while (!stopping_.load(std::memory_order_acquire))
            {
                auto maybe = CO_AWAIT acceptor_->accept();
                if (!maybe)
                    break;
                if (stopping_.load(std::memory_order_acquire))
                {
                    CO_AWAIT(*maybe)->set_closed();
                    break;
                }
                CO_AWAIT callback_(std::move(*maybe));
            }
            running_.store(false, std::memory_order_release);
            stopped_.set();
            CO_RETURN;
        }

        std::shared_ptr<::streaming::stream_acceptor> acceptor_;
        std::shared_ptr<rpc::service> service_;
        rpc::executor_ptr executor_;
        stream_callback callback_;
        std::shared_ptr<void> owner_;
        uint16_t port_{0};
        rpc::event stopped_;
        std::atomic<bool> running_{false};
        std::atomic<bool> stopping_{false};
    };

    struct stream_accept_result
    {
        int error_code{rpc::error::OK()};
        std::shared_ptr<stream_accept_handle> handle;
    };

    // Start a raw stream accept loop and keep the service/owner lifetimes tied
    // to the returned handle.
    inline stream_accept_result accept_streams(
        std::shared_ptr<::streaming::stream_acceptor> acceptor,
        stream_callback callback,
        const rpc::connection_factory_config::stream_factory_options& options,
        std::shared_ptr<rpc::service> service = {},
        std::shared_ptr<void> owner = {},
        uint16_t port = 0)
    {
        auto resolved_service = ensure_service(options, std::move(service), "stream_accept_service");
        if (!resolved_service)
            return {rpc::error::INVALID_DATA(), {}};
        auto handle = std::make_shared<stream_accept_handle>(
            std::move(acceptor), std::move(resolved_service), std::move(callback), std::move(owner), port);
        if (!handle->start())
            return {rpc::error::TRANSPORT_ERROR(), {}};
        return {rpc::error::OK(), std::move(handle)};
    }

    // Public handle for RPC listeners created by accept_rpc_listener. It owns
    // the listener, acceptor, optional runtime owner, and service lifetime.
    class listener_handle
    {
    public:
        listener_handle(
            std::shared_ptr<rpc::service> service,
            std::shared_ptr<::streaming::stream_acceptor> acceptor,
            std::unique_ptr<::streaming::listener> listener,
            std::shared_ptr<void> owner = {},
            uint16_t port = 0)
            : service_(std::move(service))
            , acceptor_(std::move(acceptor))
            , listener_(std::move(listener))
            , owner_(std::move(owner))
            , port_(port)
        {
        }

        listener_handle(const listener_handle&) = delete;
        auto operator=(const listener_handle&) -> listener_handle& = delete;

        CORO_TASK(void) stop()
        {
            if (listener_)
            {
                CO_AWAIT listener_->stop_listening();
                listener_.reset();
            }
            acceptor_.reset();
            service_.reset();
            CO_RETURN;
        }

        [[nodiscard]] std::shared_ptr<rpc::service> service() const { return service_; }
        [[nodiscard]] uint16_t port() const noexcept { return port_; }

    private:
        std::shared_ptr<rpc::service> service_;
        std::shared_ptr<::streaming::stream_acceptor> acceptor_;
        std::unique_ptr<::streaming::listener> listener_;
        std::shared_ptr<void> owner_;
        uint16_t port_{0};
    };

    struct listener_result
    {
        int error_code{rpc::error::OK()};
        std::shared_ptr<listener_handle> handle;
    };

    // Public handle for one accepted RPC stream. It keeps the service and
    // transport alive while the caller holds the handle.
    class rpc_connection_handle
    {
    public:
        rpc_connection_handle(
            std::shared_ptr<rpc::service> service,
            std::shared_ptr<rpc::stream_transport::transport> transport,
            std::shared_ptr<void> owner = {})
            : service_(std::move(service))
            , transport_(std::move(transport))
            , owner_(std::move(owner))
        {
        }

        [[nodiscard]] std::shared_ptr<rpc::service> service() const { return service_; }
        [[nodiscard]] std::shared_ptr<rpc::stream_transport::transport> transport() const { return transport_; }

    private:
        std::shared_ptr<rpc::service> service_;
        std::shared_ptr<rpc::stream_transport::transport> transport_;
        std::shared_ptr<void> owner_;
    };

    struct rpc_accept_result
    {
        int error_code{rpc::error::OK()};
        std::shared_ptr<rpc_connection_handle> handle;
    };
} // namespace rpc::connection_factory
