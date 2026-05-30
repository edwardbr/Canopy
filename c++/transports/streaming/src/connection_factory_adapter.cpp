/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <connection_factory/stream_rpc.h>

#include <utility>

namespace rpc::connection_factory
{
    client_rpc_stream_transport_result make_client_rpc_stream_transport(
        std::shared_ptr<::streaming::stream> stream,
        const stream_rpc_connection_settings& settings,
        std::shared_ptr<rpc::service> service)
    {
        auto resolved_service = ensure_service(settings, std::move(service), "rpc_client_service");
        if (!resolved_service)
            return {rpc::error::INVALID_DATA(), {}, {}};

        auto transport = rpc::stream_transport::make_client(
            transport_name(settings.transport, "initiator_transport"),
            resolved_service,
            std::move(stream),
            transport_options(settings.transport));
        if (!transport)
            return {rpc::error::TRANSPORT_ERROR(), {}, {}};

        return {rpc::error::OK(), std::move(resolved_service), std::move(transport)};
    }

    CORO_TASK(listener_result)
    start_rpc_listener(
        std::shared_ptr<::streaming::stream_acceptor> acceptor,
        ::streaming::listener::connection_callback on_connection,
        const stream_rpc_connection_settings& settings,
        std::shared_ptr<rpc::service> service,
        std::shared_ptr<void> owner,
        uint16_t port,
        ::streaming::listener::stream_transformer transform_stream)
    {
        auto resolved_service = ensure_service(settings, std::move(service), "rpc_accept_service");
        if (!resolved_service)
            CO_RETURN listener_result{rpc::error::INVALID_DATA(), {}};

        auto listener = std::make_unique<::streaming::listener>(
            listener_name(settings.listener, "responder_listener"),
            acceptor,
            std::move(on_connection),
            std::move(transform_stream));

        if (!CO_AWAIT listener->start_listening_async(resolved_service))
            CO_RETURN listener_result{rpc::error::TRANSPORT_ERROR(), {}};

        CO_RETURN listener_result{rpc::error::OK(),
            std::make_shared<listener_handle>(
                std::move(resolved_service), std::move(acceptor), std::move(listener), std::move(owner), port)};
    }
} // namespace rpc::connection_factory
