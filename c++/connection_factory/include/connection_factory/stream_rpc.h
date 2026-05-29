/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>

#include <connection_factory/handles.h>

namespace rpc::connection_factory
{
    // This header adapts already-created streams and stream acceptors into RPC
    // transports. Base-stream factories such as tcp, spsc, and io_uring decide
    // how the stream is created; the helpers here attach Canopy RPC service
    // lifetime, transport options, listener lifetime, and generated interfaces.

    // Factory called by listener code once a stream transport has discovered
    // the remote interface. Returning service_connect_result keeps this aligned
    // with connect_to_zone without exposing the lower-level transport handshake.
    template<class Remote, class Local>
    using rpc_factory
        = std::function<CORO_TASK(rpc::service_connect_result<Local>)(rpc::shared_ptr<Remote>, std::shared_ptr<rpc::service>)>;

    using rpc_transport_observer = std::function<void(std::shared_ptr<rpc::stream_transport::transport>)>;

    // Adapt a pre-existing local interface into the factory shape expected by
    // accept_rpc_* helpers.
    template<
        class Remote,
        class Local>
    rpc_factory<
        Remote,
        Local>
    fixed_factory(rpc::shared_ptr<Local> local_interface)
    {
        return [local_interface = std::move(local_interface)](
                   rpc::shared_ptr<Remote>,
                   std::shared_ptr<rpc::service>) mutable -> CORO_TASK(rpc::service_connect_result<Local>)
        { CO_RETURN rpc::service_connect_result<Local>{rpc::error::OK(), local_interface}; };
    }

    // Attach an RPC transport to an already-created stream. Transport and
    // connection names come from the materialised factory options.
    template<
        class In,
        class Out>
    CORO_TASK(rpc::service_connect_result<Out>) connect_rpc_stream(
        rpc::shared_ptr<In> input_interface,
        std::shared_ptr<::streaming::stream> stream,
        const rpc::connection_factory_config::stream_factory_options& options,
        std::shared_ptr<rpc::service> service = {})
    {
        auto resolved_service = ensure_service(options, std::move(service), "rpc_client_service");
        if (!resolved_service)
            CO_RETURN rpc::service_connect_result<Out>{rpc::error::INVALID_DATA(), {}};
        auto transport = rpc::stream_transport::make_client(
            configured_name(options, "transport", "initiator_transport"),
            resolved_service,
            std::move(stream),
            transport_options(options));
        CO_RETURN CO_AWAIT resolved_service->template connect_to_zone<In, Out>(
            configured_name(options, "connection", "main child").c_str(), std::move(transport), std::move(input_interface));
    }

    template<
        class Remote,
        class Local>
    // Accept many RPC connections from a stream_acceptor. Per-connection stream
    // transforms, such as TLS or WebSocket upgrade, plug in here without the
    // caller constructing services, listeners, or transports by hand.
    CORO_TASK(listener_result) accept_rpc_listener(
        std::shared_ptr<::streaming::stream_acceptor> acceptor,
        rpc_factory<
            Remote,
            Local> factory,
        const rpc::connection_factory_config::stream_factory_options& options,
        std::shared_ptr<rpc::service> service = {},
        std::shared_ptr<void> owner = {},
        uint16_t port = 0,
        rpc_transport_observer observe_transport = {},
        ::streaming::listener::stream_transformer transform_stream = {})
    {
        auto resolved_service = ensure_service(options, std::move(service), "rpc_accept_service");
        if (!resolved_service)
            CO_RETURN listener_result{rpc::error::INVALID_DATA(), {}};
        const auto stream_options = transport_options(options);
        auto listener = std::make_unique<::streaming::listener>(
            configured_name(options, "listener", "responder_listener"),
            acceptor,
            [factory = std::move(factory), stream_options, observe_transport = std::move(observe_transport)](
                const std::string& name,
                std::shared_ptr<rpc::service> svc,
                std::shared_ptr<::streaming::stream> stream) mutable -> CORO_TASK(void)
            {
                auto transport = rpc::stream_transport::create<Remote, Local>(
                    name, std::move(svc), std::move(stream), factory, stream_options);
                if (observe_transport)
                    observe_transport(transport);
                CO_RETURN;
            },
            std::move(transform_stream));

        if (!CO_AWAIT listener->start_listening_async(resolved_service))
            CO_RETURN listener_result{rpc::error::TRANSPORT_ERROR(), {}};

        CO_RETURN listener_result{rpc::error::OK(),
            std::make_shared<listener_handle>(
                std::move(resolved_service), std::move(acceptor), std::move(listener), std::move(owner), port)};
    }

    template<
        class Remote,
        class Local>
    CORO_TASK(listener_result)
    accept_rpc_listener(
        rpc::shared_ptr<Local> local_interface,
        std::shared_ptr<::streaming::stream_acceptor> acceptor,
        const rpc::connection_factory_config::stream_factory_options& options,
        std::shared_ptr<rpc::service> service = {},
        std::shared_ptr<void> owner = {},
        uint16_t port = 0,
        rpc_transport_observer observe_transport = {},
        ::streaming::listener::stream_transformer transform_stream = {})
    {
        CO_RETURN CO_AWAIT accept_rpc_listener<Remote, Local>(
            std::move(acceptor),
            fixed_factory<Remote, Local>(std::move(local_interface)),
            options,
            std::move(service),
            std::move(owner),
            port,
            std::move(observe_transport),
            std::move(transform_stream));
    }

    template<
        class Remote,
        class Local>
    // Accept a single already-created stream as an RPC connection.
    CORO_TASK(rpc_accept_result) accept_rpc_stream(
        std::shared_ptr<::streaming::stream> stream,
        rpc_factory<
            Remote,
            Local> factory,
        const rpc::connection_factory_config::stream_factory_options& options,
        std::shared_ptr<rpc::service> service = {},
        std::shared_ptr<void> owner = {})
    {
        auto resolved_service = ensure_service(options, std::move(service), "rpc_accept_service");
        if (!resolved_service)
            CO_RETURN rpc_accept_result{rpc::error::INVALID_DATA(), {}};
        auto transport = std::static_pointer_cast<rpc::stream_transport::transport>(
            CO_AWAIT resolved_service->template make_acceptor<Remote, Local>(
                configured_name(options, "transport", "responder_transport"),
                rpc::stream_transport::transport_factory(std::move(stream), transport_options(options)),
                std::move(factory)));

        if (!transport)
            CO_RETURN rpc_accept_result{rpc::error::TRANSPORT_ERROR(), {}};

        const auto error_code = CO_AWAIT transport->accept();
        if (error_code != rpc::error::OK())
            CO_RETURN rpc_accept_result{error_code, {}};

        CO_RETURN rpc_accept_result{rpc::error::OK(),
            std::make_shared<rpc_connection_handle>(std::move(resolved_service), std::move(transport), std::move(owner))};
    }

    template<
        class Remote,
        class Local>
    CORO_TASK(rpc_accept_result)
    accept_rpc_stream(
        rpc::shared_ptr<Local> local_interface,
        std::shared_ptr<::streaming::stream> stream,
        const rpc::connection_factory_config::stream_factory_options& options,
        std::shared_ptr<rpc::service> service = {},
        std::shared_ptr<void> owner = {})
    {
        CO_RETURN CO_AWAIT accept_rpc_stream<Remote, Local>(
            std::move(stream),
            fixed_factory<Remote, Local>(std::move(local_interface)),
            options,
            std::move(service),
            std::move(owner));
    }
} // namespace rpc::connection_factory
