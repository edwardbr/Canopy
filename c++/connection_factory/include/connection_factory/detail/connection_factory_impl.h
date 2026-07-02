/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <utility>

#include <connection_factory/detail/stream_rpc.h>
#include <streaming/listener.h>

#ifdef CANOPY_CONNECTION_FACTORY_HAS_LOCAL
#  include <transports/local/transport.h>
#endif

namespace rpc::connection_factory
{
    namespace detail
    {
        auto make_accept_stream_transformer(
            connection_settings settings,
            size_t first_layer,
            std::shared_ptr<rpc::service> service,
            context factory_context) -> ::streaming::listener::stream_transformer;

        struct native_transport_connect_context
        {
            int error_code{rpc::error::OK()};
            std::shared_ptr<rpc::service> service;
            std::shared_ptr<rpc::transport> transport;
            std::string service_proxy_name;
        };

        native_transport_connect_context make_native_transport_connect_context(
            const transport_selection_result& transport,
            const connection_settings& settings,
            std::shared_ptr<rpc::service> service);

        template<
            class In,
            class Out>
        CORO_TASK(rpc::service_connect_result<Out>)
        connect_native_transport(
            rpc::shared_ptr<In> input_interface,
            transport_selection_result transport,
            connection_settings settings,
            std::shared_ptr<rpc::service> service,
            context factory_context)
        {
            if (!transport.settings)
                CO_RETURN rpc::service_connect_result<Out>{rpc::error::INVALID_DATA(), {}};

            if (transport.type == "local")
            {
                // Local child-zone transports need the explicit child factory
                // callback supplied to connect_local_child_rpc. A plain
                // connect_rpc call does not know how to construct the child
                // zone's local object.
                CO_RETURN rpc::service_connect_result<Out>{rpc::error::INVALID_DATA(), {}};
            }

            (void)factory_context;
            auto connect_context = make_native_transport_connect_context(transport, settings, std::move(service));
            if (connect_context.error_code != rpc::error::OK())
                CO_RETURN rpc::service_connect_result<Out>{connect_context.error_code, {}};
            if (!connect_context.service || !connect_context.transport)
                CO_RETURN rpc::service_connect_result<Out>{rpc::error::INVALID_DATA(), {}};

            CO_RETURN CO_AWAIT connect_context.service->template connect_to_zone<In, Out>(
                std::move(connect_context.service_proxy_name),
                std::move(connect_context.transport),
                std::move(input_interface));
        }

        template<class In, class Out> struct local_child_entry_point
        {
            rpc_factory<In, Out> child_factory;

            auto operator()(
                rpc::shared_ptr<In> remote_interface,
                std::shared_ptr<rpc::child_service> child_service) const -> CORO_TASK(rpc::service_connect_result<Out>)
            {
                CO_RETURN CO_AWAIT child_factory(std::move(remote_interface), std::move(child_service));
            }
        };
    } // namespace detail

#ifdef CANOPY_CONNECTION_FACTORY_HAS_LOCAL
    template<
        class In,
        class Out>
    CORO_TASK(rpc::service_connect_result<Out>)
    connect_local_child_rpc(
        rpc::shared_ptr<In> input_interface,
        rpc_factory<
            In,
            Out> child_factory,
        connection_settings settings,
        std::shared_ptr<rpc::service> service = {})
    {
        if (!child_factory)
            CO_RETURN rpc::service_connect_result<Out>{rpc::error::INVALID_DATA(), {}};

        auto transport = detail::transport_from_connection(settings);
        if (transport.error_code != rpc::error::OK())
            CO_RETURN rpc::service_connect_result<Out>{transport.error_code, {}};
        if (transport.type != "local" || !transport.settings)
            CO_RETURN rpc::service_connect_result<Out>{rpc::error::INVALID_DATA(), {}};

        auto local = detail::make_native_transport_connect_context(transport, settings, std::move(service));
        if (local.error_code != rpc::error::OK())
            CO_RETURN rpc::service_connect_result<Out>{local.error_code, {}};
        if (!local.service || !local.transport)
            CO_RETURN rpc::service_connect_result<Out>{rpc::error::INVALID_DATA(), {}};

        auto local_transport = std::dynamic_pointer_cast<rpc::local::child_transport>(local.transport);
        if (!local_transport)
            CO_RETURN rpc::service_connect_result<Out>{rpc::error::INVALID_DATA(), {}};

        std::function<CORO_TASK(rpc::service_connect_result<Out>)(rpc::shared_ptr<In>, std::shared_ptr<rpc::child_service>)> entry_point
            = detail::local_child_entry_point<In, Out>{std::move(child_factory)};
        local_transport->template set_child_entry_point<In, Out>(std::move(entry_point));

        CO_RETURN CO_AWAIT local.service->template connect_to_zone<In, Out>(
            std::move(local.service_proxy_name), std::move(local_transport), std::move(input_interface));
    }
#endif

    template<
        class In,
        class Out>
    CORO_TASK(rpc::service_connect_result<Out>)
    connect_rpc(
        rpc::shared_ptr<In> input_interface,
        connection_settings settings,
        std::shared_ptr<rpc::service> service,
        context factory_context)
    {
        auto transport = detail::transport_from_connection(settings);
        if (transport.error_code != rpc::error::OK())
            CO_RETURN rpc::service_connect_result<Out>{transport.error_code, {}};

        if (transport.type != "stream_rpc")
        {
            CO_RETURN CO_AWAIT detail::connect_native_transport<In, Out>(
                std::move(input_interface), std::move(transport), settings, std::move(service), factory_context);
        }

        auto rpc_settings = detail::resolve_stream_rpc_settings(settings);
        if (rpc_settings.error_code != rpc::error::OK())
            CO_RETURN rpc::service_connect_result<Out>{rpc_settings.error_code, {}};

        auto resolved_service = ensure_service(rpc_settings.settings, std::move(service), "layered_rpc_client");
        if (!resolved_service)
            CO_RETURN rpc::service_connect_result<Out>{rpc::error::INVALID_DATA(), {}};

        auto stream = CO_AWAIT connect_stream(settings, resolved_service, std::move(factory_context));
        if (stream.error_code != rpc::error::OK())
            CO_RETURN rpc::service_connect_result<Out>{stream.error_code, {}};

        CO_RETURN CO_AWAIT connect_rpc_stream<In, Out>(
            std::move(input_interface), std::move(stream.stream), rpc_settings.settings, std::move(resolved_service));
    }

    template<
        class Remote,
        class Local>
    CORO_TASK(accept_result)
    accept_rpc(
        rpc_factory<
            Remote,
            Local> factory,
        connection_settings settings,
        std::shared_ptr<rpc::service> service,
        context factory_context,
        rpc_transport_observer observe_transport)
    {
        auto transport = detail::transport_from_connection(settings);
        if (transport.error_code != rpc::error::OK())
            CO_RETURN accept_result{transport.error_code, {}, {}};
        if (transport.type != "stream_rpc")
            CO_RETURN accept_result{rpc::error::INVALID_DATA(), {}, {}};

        auto rpc_settings = detail::resolve_stream_rpc_settings(settings);
        if (rpc_settings.error_code != rpc::error::OK())
            CO_RETURN accept_result{rpc_settings.error_code, {}, {}};

        auto resolved_service = ensure_service(rpc_settings.settings, std::move(service), "layered_rpc_accept");
        if (!resolved_service)
            CO_RETURN accept_result{rpc::error::INVALID_DATA(), {}, {}};

        auto acceptor = CO_AWAIT open_stream_acceptor(settings, resolved_service, factory_context);
        if (acceptor.error_code == rpc::error::OK())
        {
            auto stream_transformer
                = detail::make_accept_stream_transformer(settings, 1, resolved_service, factory_context);
            auto listener = CO_AWAIT accept_rpc_listener<Remote, Local>(
                std::move(acceptor.acceptor),
                std::move(factory),
                rpc_settings.settings,
                std::move(resolved_service),
                std::move(acceptor.owner),
                acceptor.port,
                std::move(observe_transport),
                std::move(stream_transformer));
            CO_RETURN accept_result{listener.error_code, std::move(listener.handle), {}};
        }
        if (acceptor.error_code != rpc::error::INVALID_DATA())
            CO_RETURN accept_result{acceptor.error_code, {}, {}};

        auto accepted_stream = CO_AWAIT accept_stream(settings, resolved_service, std::move(factory_context));
        if (accepted_stream.error_code != rpc::error::OK())
            CO_RETURN accept_result{accepted_stream.error_code, {}, {}};

        auto connection = CO_AWAIT accept_rpc_stream<Remote, Local>(
            std::move(accepted_stream.stream), std::move(factory), rpc_settings.settings, std::move(resolved_service));
        CO_RETURN accept_result{connection.error_code, {}, std::move(connection.handle)};
    }

    template<
        class Remote,
        class Local>
    CORO_TASK(accept_result)
    accept_rpc(
        rpc::shared_ptr<Local> local_interface,
        connection_settings settings,
        std::shared_ptr<rpc::service> service,
        context factory_context,
        rpc_transport_observer observe_transport)
    {
        CO_RETURN CO_AWAIT accept_rpc<Remote, Local>(
            fixed_factory<Remote, Local>(std::move(local_interface)),
            std::move(settings),
            std::move(service),
            std::move(factory_context),
            std::move(observe_transport));
    }
} // namespace rpc::connection_factory
