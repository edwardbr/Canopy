/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

#include <connection_factory/service.h>

namespace streaming
{
    class listener;
    class stream;
    class stream_acceptor;
} // namespace streaming

namespace rpc::connection_factory
{
    // These handles are the ownership boundary returned by the simplified
    // factories. They hide transports/listeners from callers that only need a
    // stream or RPC endpoint, while still keeping the service, acceptor, and
    // optional runtime owner alive for as long as the handle is retained.

    std::shared_ptr<::streaming::stream> keep_owner(
        std::shared_ptr<::streaming::stream> stream,
        std::shared_ptr<void> owner);

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
            uint16_t port = 0);

        [[nodiscard]] bool start();

        CORO_TASK(void) stop();

        [[nodiscard]] uint16_t port() const noexcept;

    private:
        CORO_TASK(void) run(std::shared_ptr<stream_accept_handle> self);

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
    stream_accept_result accept_streams(
        std::shared_ptr<::streaming::stream_acceptor> acceptor,
        stream_callback callback,
        const stream_rpc_connection_settings& settings,
        std::shared_ptr<rpc::service> service = {},
        std::shared_ptr<void> owner = {},
        uint16_t port = 0);

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
            uint16_t port = 0);
        ~listener_handle();

        listener_handle(const listener_handle&) = delete;
        auto operator=(const listener_handle&) -> listener_handle& = delete;

        CORO_TASK(void) stop();

        [[nodiscard]] std::shared_ptr<rpc::service> service() const;
        [[nodiscard]] uint16_t port() const noexcept;

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
            std::shared_ptr<void> owner = {});

        [[nodiscard]] std::shared_ptr<rpc::service> service() const;
        [[nodiscard]] std::shared_ptr<rpc::stream_transport::transport> transport() const;

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
