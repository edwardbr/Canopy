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

#ifdef CANOPY_CONNECTION_FACTORY_HAS_SGX_COROUTINE
#  include <transports/sgx_coroutine/host/rpc_bootstrap.h>
#endif

namespace rpc::connection_factory
{
    namespace detail
    {
        auto make_accept_stream_transformer(
            const connection_settings& settings,
            size_t first_layer,
            const context& factory_context) -> ::streaming::listener::stream_transformer;

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

#ifdef CANOPY_CONNECTION_FACTORY_HAS_SGX_COROUTINE
        rpc::sgx_coroutine_transport::host::rpc_bootstrap_context make_sgx_coroutine_bootstrap_context(
            const transport_selection_result& transport,
            const connection_settings& settings,
            std::shared_ptr<rpc::service> service,
            const context& factory_context,
            rpc::shared_ptr<rpc::i_noop> input_interface);

        template<
            class In,
            class Out>
        CORO_TASK(rpc::service_connect_result<Out>)
        connect_sgx_coroutine_transport(
            rpc::shared_ptr<In> input_interface,
            const transport_selection_result& transport,
            const connection_settings& settings,
            std::shared_ptr<rpc::service> service,
            const context& factory_context)
        {
            rpc::shared_ptr<rpc::i_noop> erased_interface;
            if (input_interface)
            {
                erased_interface = CO_AWAIT rpc::dynamic_pointer_cast<rpc::i_noop>(input_interface);
                if (!erased_interface)
                    CO_RETURN rpc::service_connect_result<Out>{rpc::error::INVALID_CAST(), {}};
            }

            auto bootstrap = make_sgx_coroutine_bootstrap_context(
                transport, settings, std::move(service), factory_context, std::move(erased_interface));
            CO_RETURN CO_AWAIT rpc::sgx_coroutine_transport::host::connect_rpc_transport<Out>(std::move(bootstrap));
        }
#endif

#ifdef CANOPY_CONNECTION_FACTORY_HAS_SGX_BLOCKING
        template<
            class In,
            class Out>
        CORO_TASK(rpc::service_connect_result<Out>)
        connect_sgx_blocking_transport(
            rpc::shared_ptr<In> input_interface,
            const transport_selection_result& transport,
            const connection_settings& settings,
            std::shared_ptr<rpc::service> service)
        {
            auto connect_context = make_native_transport_connect_context(transport, settings, std::move(service));
            if (connect_context.error_code != rpc::error::OK())
                CO_RETURN rpc::service_connect_result<Out>{connect_context.error_code, {}};
            if (!connect_context.service || !connect_context.transport)
                CO_RETURN rpc::service_connect_result<Out>{rpc::error::INVALID_DATA(), {}};

            CO_RETURN CO_AWAIT connect_context.service->template connect_to_zone<In, Out>(
                connect_context.service_proxy_name.c_str(), std::move(connect_context.transport), std::move(input_interface));
        }
#endif

        template<
            class In,
            class Out>
        CORO_TASK(rpc::service_connect_result<Out>)
        connect_native_transport(
            rpc::shared_ptr<In> input_interface,
            const transport_selection_result& transport,
            const connection_settings& settings,
            std::shared_ptr<rpc::service> service,
            const context& factory_context)
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

#ifdef CANOPY_CONNECTION_FACTORY_HAS_SGX_COROUTINE
            if (transport.type == "sgx_coroutine")
            {
                CO_RETURN CO_AWAIT connect_sgx_coroutine_transport<In, Out>(
                    std::move(input_interface), transport, settings, std::move(service), factory_context);
            }
#endif

#ifdef CANOPY_CONNECTION_FACTORY_HAS_SGX_BLOCKING
            if (transport.type == "sgx_blocking")
            {
                (void)factory_context;
                CO_RETURN CO_AWAIT connect_sgx_blocking_transport<In, Out>(
                    std::move(input_interface), transport, settings, std::move(service));
            }
#endif

            (void)factory_context;
            auto connect_context = make_native_transport_connect_context(transport, settings, std::move(service));
            if (connect_context.error_code != rpc::error::OK())
                CO_RETURN rpc::service_connect_result<Out>{connect_context.error_code, {}};
            if (!connect_context.service || !connect_context.transport)
                CO_RETURN rpc::service_connect_result<Out>{rpc::error::INVALID_DATA(), {}};

            CO_RETURN CO_AWAIT connect_context.service->template connect_to_zone<In, Out>(
                connect_context.service_proxy_name.c_str(), std::move(connect_context.transport), std::move(input_interface));
        }
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
        const connection_settings& settings,
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
            = [child_factory = std::move(child_factory)](
                  rpc::shared_ptr<In> remote_interface,
                  std::shared_ptr<rpc::child_service> child_service) mutable -> CORO_TASK(rpc::service_connect_result<Out>)
        { CO_RETURN CO_AWAIT child_factory(std::move(remote_interface), std::move(child_service)); };
        local_transport->template set_child_entry_point<In, Out>(std::move(entry_point));

        CO_RETURN CO_AWAIT local.service->template connect_to_zone<In, Out>(
            local.service_proxy_name.c_str(), std::move(local_transport), std::move(input_interface));
    }
#endif

    template<
        class In,
        class Out>
    CORO_TASK(rpc::service_connect_result<Out>)
    connect_rpc(
        rpc::shared_ptr<In> input_interface,
        const connection_settings& settings,
        std::shared_ptr<rpc::service> service,
        const context& factory_context)
    {
        auto transport = detail::transport_from_connection(settings);
        if (transport.error_code != rpc::error::OK())
            CO_RETURN rpc::service_connect_result<Out>{transport.error_code, {}};

        if (transport.type != "stream_rpc")
        {
            CO_RETURN CO_AWAIT detail::connect_native_transport<In, Out>(
                std::move(input_interface), transport, settings, std::move(service), factory_context);
        }

        auto rpc_settings = detail::resolve_stream_rpc_settings(settings);
        if (rpc_settings.error_code != rpc::error::OK())
            CO_RETURN rpc::service_connect_result<Out>{rpc_settings.error_code, {}};

        auto resolved_service = ensure_service(rpc_settings.settings, std::move(service), "layered_rpc_client");
        if (!resolved_service)
            CO_RETURN rpc::service_connect_result<Out>{rpc::error::INVALID_DATA(), {}};

        auto stream = CO_AWAIT connect_stream(settings, resolved_service, factory_context);
        if (stream.error_code != rpc::error::OK())
            CO_RETURN rpc::service_connect_result<Out>{stream.error_code, {}};

        CO_RETURN CO_AWAIT connect_rpc_stream<In, Out>(
            std::move(input_interface), std::move(stream.stream), rpc_settings.settings, std::move(resolved_service));
    }

    template<
        class Remote,
        class Local>
    CORO_TASK(layered_accept_result)
    accept_rpc(
        rpc_factory<
            Remote,
            Local> factory,
        const connection_settings& settings,
        std::shared_ptr<rpc::service> service,
        const context& factory_context,
        rpc_transport_observer observe_transport)
    {
        auto transport = detail::transport_from_connection(settings);
        if (transport.error_code != rpc::error::OK())
            CO_RETURN layered_accept_result{transport.error_code, {}, {}};
        if (transport.type != "stream_rpc")
            CO_RETURN layered_accept_result{rpc::error::INVALID_DATA(), {}, {}};

        auto rpc_settings = detail::resolve_stream_rpc_settings(settings);
        if (rpc_settings.error_code != rpc::error::OK())
            CO_RETURN layered_accept_result{rpc_settings.error_code, {}, {}};

        auto resolved_service = ensure_service(rpc_settings.settings, std::move(service), "layered_rpc_accept");
        if (!resolved_service)
            CO_RETURN layered_accept_result{rpc::error::INVALID_DATA(), {}, {}};

        auto acceptor = CO_AWAIT open_stream_acceptor(settings, resolved_service, factory_context);
        if (acceptor.error_code == rpc::error::OK())
        {
            auto listener = CO_AWAIT accept_rpc_listener<Remote, Local>(
                std::move(acceptor.acceptor),
                std::move(factory),
                rpc_settings.settings,
                std::move(resolved_service),
                std::move(acceptor.owner),
                acceptor.port,
                std::move(observe_transport),
                detail::make_accept_stream_transformer(settings, 1, factory_context));
            CO_RETURN layered_accept_result{listener.error_code, std::move(listener.handle), {}};
        }
        if (acceptor.error_code != rpc::error::INVALID_DATA())
            CO_RETURN layered_accept_result{acceptor.error_code, {}, {}};

        auto accepted_stream = CO_AWAIT accept_stream(settings, resolved_service, factory_context);
        if (accepted_stream.error_code != rpc::error::OK())
            CO_RETURN layered_accept_result{accepted_stream.error_code, {}, {}};

        auto connection = CO_AWAIT accept_rpc_stream<Remote, Local>(
            std::move(accepted_stream.stream), std::move(factory), rpc_settings.settings, std::move(resolved_service));
        CO_RETURN layered_accept_result{connection.error_code, {}, std::move(connection.handle)};
    }

    template<
        class Remote,
        class Local>
    CORO_TASK(layered_accept_result)
    accept_rpc(
        rpc::shared_ptr<Local> local_interface,
        const connection_settings& settings,
        std::shared_ptr<rpc::service> service,
        const context& factory_context,
        rpc_transport_observer observe_transport)
    {
        CO_RETURN CO_AWAIT accept_rpc<Remote, Local>(
            fixed_factory<Remote, Local>(std::move(local_interface)),
            settings,
            std::move(service),
            factory_context,
            std::move(observe_transport));
    }
} // namespace rpc::connection_factory
