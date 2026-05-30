/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <typeindex>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include <json/config.h>
#include <connection_factory/stream_rpc.h>
#include <connection_factory_config/connection_factory_config.h>
#include <streaming/stream_layers.h>

#ifdef CANOPY_CONNECTION_FACTORY_HAS_LOCAL
#  include <transports/local/transport.h>
#endif

#ifdef CANOPY_CONNECTION_FACTORY_HAS_SGX_COROUTINE
#  include <transports/sgx_coroutine/host/connect.h>
#endif

#ifdef CANOPY_CONNECTION_FACTORY_HAS_SPSC
#  include <connection_factory/spsc_queue.h>
#endif

#ifdef CANOPY_CONNECTION_FACTORY_HAS_TLS
#  include <streaming/secure_stream.h>
#endif

#ifdef CANOPY_CONNECTION_FACTORY_HAS_ATTESTATION
#  include <security/attestation/service.h>
#endif

namespace rpc::connection_factory
{
    enum class layer_direction
    {
        connect,
        accept,
    };

    class layered_connection_context;

    struct stream_acceptor_result
    {
        int error_code{rpc::error::OK()};
        std::shared_ptr<::streaming::stream_acceptor> acceptor;
        std::shared_ptr<void> owner;
        uint16_t port{0};
    };

    struct layered_accept_result
    {
        int error_code{rpc::error::OK()};
        std::shared_ptr<listener_handle> listener;
        std::shared_ptr<rpc_connection_handle> connection;
    };

    using stream_layer_builder = std::function<CORO_TASK(stream_result)(
        std::shared_ptr<::streaming::stream>, const json::v1::object&, layer_direction, const layered_connection_context&)>;

    using base_stream_connect_builder = std::function<CORO_TASK(stream_result)(
        const json::v1::object&, std::shared_ptr<rpc::service>, const layered_connection_context&)>;

    using base_stream_acceptor_builder = std::function<CORO_TASK(stream_acceptor_result)(
        const json::v1::object&, std::shared_ptr<rpc::service>, const layered_connection_context&)>;

    using base_stream_accept_builder = std::function<CORO_TASK(stream_result)(
        const json::v1::object&, std::shared_ptr<rpc::service>, const layered_connection_context&)>;

    class layered_connection_context
    {
    public:
        layered_connection_context();
        ~layered_connection_context();

        layered_connection_context(const layered_connection_context&) = default;
        layered_connection_context(layered_connection_context&&) noexcept = default;
        auto operator=(const layered_connection_context&) -> layered_connection_context& = default;
        auto operator=(layered_connection_context&&) noexcept -> layered_connection_context& = default;

        // Generic dependency registration used by built-in and application
        // factories. The optional name lets one context hold multiple
        // instances of the same dependency type, for example several
        // attestation services with different policies.
        template<class T>
        void set_dependency(
            std::shared_ptr<T> dependency,
            std::string name = {})
        {
            using dependency_type = std::remove_cv_t<T>;
            set_dependency_impl(
                std::type_index(typeid(dependency_type)),
                std::move(name),
                std::static_pointer_cast<void>(std::move(dependency)));
        }

        template<class T>
        void set_dependency_value(
            T dependency,
            std::string name = {})
        {
            using dependency_type = std::remove_cv_t<std::remove_reference_t<T>>;
            set_dependency<dependency_type>(std::make_shared<dependency_type>(std::move(dependency)), std::move(name));
        }

        template<class T> auto get_dependency(std::string name = {}) const -> std::shared_ptr<std::remove_cv_t<T>>
        {
            using dependency_type = std::remove_cv_t<T>;
            return std::static_pointer_cast<dependency_type>(
                get_dependency_impl(std::type_index(typeid(dependency_type)), name));
        }

        template<class T>
        auto get_dependencies() const -> std::unordered_map<
            std::string,
            std::shared_ptr<std::remove_cv_t<T>>>
        {
            using dependency_type = std::remove_cv_t<T>;
            std::unordered_map<std::string, std::shared_ptr<dependency_type>> result;
            auto dependencies = get_dependencies_impl(std::type_index(typeid(dependency_type)));
            for (auto& [name, dependency] : dependencies)
                result.emplace(std::move(name), std::static_pointer_cast<dependency_type>(std::move(dependency)));
            return result;
        }

#ifdef CANOPY_CONNECTION_FACTORY_HAS_SPSC
        void set_spsc_queues(rpc::spsc_queue::queue_pair queues);
#endif

#ifdef CANOPY_CONNECTION_FACTORY_HAS_TLS
        void set_tls_client_context(std::shared_ptr<::streaming::secure::client_context> context);
        void set_tls_server_context(std::shared_ptr<::streaming::secure::context> context);
#endif

#ifdef CANOPY_CONNECTION_FACTORY_HAS_SPSC_WRAPPING
        void set_stream_scheduler(std::shared_ptr<rpc::executor> executor);
#endif

#ifdef CANOPY_CONNECTION_FACTORY_HAS_ATTESTATION
        void set_attestation_service(std::shared_ptr<canopy::security::attestation::attestation_service> service);
        void register_attestation_service(
            std::string name,
            std::shared_ptr<canopy::security::attestation::attestation_service> service);
#endif

        void register_connect_base_stream(
            std::string type,
            base_stream_connect_builder builder);
        void register_accept_base_stream(
            std::string type,
            base_stream_acceptor_builder builder);
        void register_accept_single_stream(
            std::string type,
            base_stream_accept_builder builder);
        void register_stream_layer(
            std::string type,
            stream_layer_builder builder);

    private:
        struct impl;
        std::shared_ptr<impl> impl_;

        void set_dependency_impl(
            std::type_index type,
            std::string name,
            std::shared_ptr<void> dependency);
        [[nodiscard]] auto get_dependency_impl(
            std::type_index type,
            const std::string& name) const -> std::shared_ptr<void>;
        [[nodiscard]] auto get_dependencies_impl(std::type_index type) const -> std::unordered_map<
            std::string,
            std::shared_ptr<void>>;

        friend auto connect_base_stream(
            const rpc::stream_layers::stream_layer_settings&,
            std::shared_ptr<rpc::service>,
            const layered_connection_context&) -> CORO_TASK(stream_result);
        friend auto accept_base_streams(
            const rpc::stream_layers::stream_layer_settings&,
            std::shared_ptr<rpc::service>,
            const layered_connection_context&) -> CORO_TASK(stream_acceptor_result);
        friend auto accept_single_base_stream(
            const rpc::stream_layers::stream_layer_settings&,
            std::shared_ptr<rpc::service>,
            const layered_connection_context&) -> CORO_TASK(stream_result);
        friend auto apply_stream_layer(
            std::shared_ptr<::streaming::stream>,
            const rpc::stream_layers::stream_layer_settings&,
            layer_direction,
            const layered_connection_context&) -> CORO_TASK(stream_result);
    };

    auto connect_base_stream(
        const rpc::stream_layers::stream_layer_settings& layer,
        std::shared_ptr<rpc::service> service,
        const layered_connection_context& context) -> CORO_TASK(stream_result);

    auto accept_base_streams(
        const rpc::stream_layers::stream_layer_settings& layer,
        std::shared_ptr<rpc::service> service,
        const layered_connection_context& context) -> CORO_TASK(stream_acceptor_result);

    auto accept_single_base_stream(
        const rpc::stream_layers::stream_layer_settings& layer,
        std::shared_ptr<rpc::service> service,
        const layered_connection_context& context) -> CORO_TASK(stream_result);

    auto apply_stream_layers(
        std::shared_ptr<::streaming::stream> stream,
        const rpc::connection_factory_config::connection_settings& settings,
        size_t first_layer,
        layer_direction direction,
        const layered_connection_context& context) -> CORO_TASK(stream_result);

    namespace detail
    {
        struct native_transport_connect_context
        {
            int error_code{rpc::error::OK()};
            std::shared_ptr<rpc::service> service;
            std::shared_ptr<rpc::transport> transport;
            std::string service_proxy_name;
        };

#ifdef CANOPY_CONNECTION_FACTORY_HAS_LOCAL
        struct local_child_connect_context
        {
            int error_code{rpc::error::OK()};
            std::shared_ptr<rpc::service> service;
            std::shared_ptr<rpc::local::child_transport> transport;
            std::string service_proxy_name;
        };

        local_child_connect_context make_local_child_connect_context(
            const rpc::connection_factory_config::connection_settings& settings,
            std::shared_ptr<rpc::service> service);
#endif

#ifdef CANOPY_CONNECTION_FACTORY_HAS_SGX_COROUTINE
        struct sgx_coroutine_connect_context
        {
            int error_code{rpc::error::OK()};
            std::shared_ptr<rpc::service> service;
            std::shared_ptr<rpc::transport> transport;
            std::string service_proxy_name;
            rpc::io_uring::host_controller::options controller_options;
        };

        sgx_coroutine_connect_context make_sgx_coroutine_connect_context(
            const rpc::connection_factory_config::typed_settings& transport_settings,
            const rpc::connection_factory_config::connection_settings& settings,
            std::shared_ptr<rpc::service> service,
            const layered_connection_context& context);

        template<
            class In,
            class Out>
        CORO_TASK(rpc::service_connect_result<Out>)
        connect_sgx_coroutine_transport(
            rpc::shared_ptr<In> input_interface,
            const rpc::connection_factory_config::typed_settings& transport_settings,
            const rpc::connection_factory_config::connection_settings& settings,
            std::shared_ptr<rpc::service> service,
            const layered_connection_context& context)
        {
            auto connect_context
                = make_sgx_coroutine_connect_context(transport_settings, settings, std::move(service), context);
            if (connect_context.error_code != rpc::error::OK())
                CO_RETURN rpc::service_connect_result<Out>{connect_context.error_code, {}};
            if (!connect_context.service || !connect_context.transport)
                CO_RETURN rpc::service_connect_result<Out>{rpc::error::INVALID_DATA(), {}};

            CO_RETURN CO_AWAIT rpc::sgx_coroutine_transport::host::connect_to_enclave_zone<In, Out>(
                connect_context.service,
                connect_context.service_proxy_name.c_str(),
                std::move(connect_context.transport),
                std::move(input_interface),
                std::move(connect_context.controller_options));
        }
#endif

#ifdef CANOPY_CONNECTION_FACTORY_HAS_SGX_BLOCKING
        native_transport_connect_context make_sgx_blocking_connect_context(
            const rpc::connection_factory_config::typed_settings& transport_settings,
            const rpc::connection_factory_config::connection_settings& settings,
            std::shared_ptr<rpc::service> service);

        template<
            class In,
            class Out>
        CORO_TASK(rpc::service_connect_result<Out>)
        connect_sgx_blocking_transport(
            rpc::shared_ptr<In> input_interface,
            const rpc::connection_factory_config::typed_settings& transport_settings,
            const rpc::connection_factory_config::connection_settings& settings,
            std::shared_ptr<rpc::service> service)
        {
            auto connect_context = make_sgx_blocking_connect_context(transport_settings, settings, std::move(service));
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
            const rpc::connection_factory_config::connection_settings& settings,
            std::shared_ptr<rpc::service> service,
            const layered_connection_context& context)
        {
            if (!transport.settings)
                CO_RETURN rpc::service_connect_result<Out>{rpc::error::INVALID_DATA(), {}};

#ifdef CANOPY_CONNECTION_FACTORY_HAS_SGX_COROUTINE
            if (transport.type == "sgx_coroutine")
            {
                CO_RETURN CO_AWAIT connect_sgx_coroutine_transport<In, Out>(
                    std::move(input_interface), *transport.settings, settings, std::move(service), context);
            }
#endif

#ifdef CANOPY_CONNECTION_FACTORY_HAS_SGX_BLOCKING
            if (transport.type == "sgx_blocking")
            {
                (void)context;
                CO_RETURN CO_AWAIT connect_sgx_blocking_transport<In, Out>(
                    std::move(input_interface), *transport.settings, settings, std::move(service));
            }
#endif

#if !defined(CANOPY_CONNECTION_FACTORY_HAS_SGX_COROUTINE) && !defined(CANOPY_CONNECTION_FACTORY_HAS_SGX_BLOCKING)
            (void)input_interface;
            (void)settings;
            (void)service;
            (void)context;
#endif

            CO_RETURN rpc::service_connect_result<Out>{rpc::error::INVALID_DATA(), {}};
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
        const rpc::connection_factory_config::connection_settings& settings,
        std::shared_ptr<rpc::service> service = {})
    {
        if (!child_factory)
            CO_RETURN rpc::service_connect_result<Out>{rpc::error::INVALID_DATA(), {}};

        auto local = detail::make_local_child_connect_context(settings, std::move(service));
        if (local.error_code != rpc::error::OK())
            CO_RETURN rpc::service_connect_result<Out>{local.error_code, {}};
        if (!local.service || !local.transport)
            CO_RETURN rpc::service_connect_result<Out>{rpc::error::INVALID_DATA(), {}};

        std::function<CORO_TASK(rpc::service_connect_result<Out>)(rpc::shared_ptr<In>, std::shared_ptr<rpc::child_service>)> entry_point
            = [child_factory = std::move(child_factory)](
                  rpc::shared_ptr<In> remote_interface,
                  std::shared_ptr<rpc::child_service> child_service) mutable -> CORO_TASK(rpc::service_connect_result<Out>)
        { CO_RETURN CO_AWAIT child_factory(std::move(remote_interface), std::move(child_service)); };
        local.transport->template set_child_entry_point<In, Out>(std::move(entry_point));

        CO_RETURN CO_AWAIT local.service->template connect_to_zone<In, Out>(
            local.service_proxy_name.c_str(), std::move(local.transport), std::move(input_interface));
    }
#endif

    template<
        class In,
        class Out>
    CORO_TASK(rpc::service_connect_result<Out>)
    connect_rpc(
        rpc::shared_ptr<In> input_interface,
        const rpc::connection_factory_config::connection_settings& settings,
        std::shared_ptr<rpc::service> service = {},
        layered_connection_context context = {})
    {
        auto transport = transport_from_connection(settings);
        if (transport.error_code != rpc::error::OK())
            CO_RETURN rpc::service_connect_result<Out>{transport.error_code, {}};

        if (transport.type != "stream_rpc")
        {
            CO_RETURN CO_AWAIT detail::connect_native_transport<In, Out>(
                std::move(input_interface), transport, settings, std::move(service), context);
        }

        if (settings.stream_layers.empty())
            CO_RETURN rpc::service_connect_result<Out>{rpc::error::INVALID_DATA(), {}};

        const auto& base_layer = settings.stream_layers.front();

        auto rpc_settings = resolve_stream_rpc_settings(settings);
        if (rpc_settings.error_code != rpc::error::OK())
            CO_RETURN rpc::service_connect_result<Out>{rpc_settings.error_code, {}};

        auto resolved_service = ensure_service(rpc_settings.settings, std::move(service), "layered_rpc_client");
        if (!resolved_service)
            CO_RETURN rpc::service_connect_result<Out>{rpc::error::INVALID_DATA(), {}};

        auto stream = CO_AWAIT connect_base_stream(base_layer, resolved_service, context);
        if (stream.error_code != rpc::error::OK())
            CO_RETURN rpc::service_connect_result<Out>{stream.error_code, {}};

        auto wrapped
            = CO_AWAIT apply_stream_layers(std::move(stream.stream), settings, 1, layer_direction::connect, context);
        if (wrapped.error_code != rpc::error::OK())
            CO_RETURN rpc::service_connect_result<Out>{wrapped.error_code, {}};

        CO_RETURN CO_AWAIT connect_rpc_stream<In, Out>(
            std::move(input_interface), std::move(wrapped.stream), rpc_settings.settings, std::move(resolved_service));
    }

    template<
        class Remote,
        class Local>
    CORO_TASK(layered_accept_result)
    accept_rpc(
        rpc_factory<
            Remote,
            Local> factory,
        const rpc::connection_factory_config::connection_settings& settings,
        std::shared_ptr<rpc::service> service = {},
        layered_connection_context context = {},
        rpc_transport_observer observe_transport = {})
    {
        auto transport = transport_from_connection(settings);
        if (transport.error_code != rpc::error::OK())
            CO_RETURN layered_accept_result{transport.error_code, {}, {}};
        if (transport.type != "stream_rpc")
            CO_RETURN layered_accept_result{rpc::error::INVALID_DATA(), {}, {}};

        if (settings.stream_layers.empty())
            CO_RETURN layered_accept_result{rpc::error::INVALID_DATA(), {}, {}};

        auto rpc_settings = resolve_stream_rpc_settings(settings);
        if (rpc_settings.error_code != rpc::error::OK())
            CO_RETURN layered_accept_result{rpc_settings.error_code, {}, {}};

        auto resolved_service = ensure_service(rpc_settings.settings, std::move(service), "layered_rpc_accept");
        if (!resolved_service)
            CO_RETURN layered_accept_result{rpc::error::INVALID_DATA(), {}, {}};

        const auto& base_layer = settings.stream_layers.front();
        auto acceptor = CO_AWAIT accept_base_streams(base_layer, resolved_service, context);
        if (acceptor.error_code == rpc::error::OK())
        {
            ::streaming::listener::stream_transformer transform;
            if (settings.stream_layers.size() > 1)
            {
                transform = [settings, context](std::shared_ptr<::streaming::stream> stream)
                    -> CORO_TASK(std::optional<std::shared_ptr<::streaming::stream>>)
                {
                    auto wrapped
                        = CO_AWAIT apply_stream_layers(std::move(stream), settings, 1, layer_direction::accept, context);
                    if (wrapped.error_code != rpc::error::OK())
                        CO_RETURN std::nullopt;
                    CO_RETURN std::optional<std::shared_ptr<::streaming::stream>>(std::move(wrapped.stream));
                };
            }

            auto listener = CO_AWAIT accept_rpc_listener<Remote, Local>(
                std::move(acceptor.acceptor),
                std::move(factory),
                rpc_settings.settings,
                std::move(resolved_service),
                std::move(acceptor.owner),
                acceptor.port,
                std::move(observe_transport),
                std::move(transform));
            CO_RETURN layered_accept_result{listener.error_code, std::move(listener.handle), {}};
        }
        if (acceptor.error_code != rpc::error::INVALID_DATA())
            CO_RETURN layered_accept_result{acceptor.error_code, {}, {}};

        auto accepted_stream = CO_AWAIT accept_single_base_stream(base_layer, resolved_service, context);
        if (accepted_stream.error_code != rpc::error::OK())
            CO_RETURN layered_accept_result{accepted_stream.error_code, {}, {}};

        auto wrapped = CO_AWAIT apply_stream_layers(
            std::move(accepted_stream.stream), settings, 1, layer_direction::accept, context);
        if (wrapped.error_code != rpc::error::OK())
            CO_RETURN layered_accept_result{wrapped.error_code, {}, {}};

        auto connection = CO_AWAIT accept_rpc_stream<Remote, Local>(
            std::move(wrapped.stream), std::move(factory), rpc_settings.settings, std::move(resolved_service));
        CO_RETURN layered_accept_result{connection.error_code, {}, std::move(connection.handle)};
    }

    template<
        class Remote,
        class Local>
    CORO_TASK(layered_accept_result)
    accept_rpc(
        rpc::shared_ptr<Local> local_interface,
        const rpc::connection_factory_config::connection_settings& settings,
        std::shared_ptr<rpc::service> service = {},
        layered_connection_context context = {},
        rpc_transport_observer observe_transport = {})
    {
        CO_RETURN CO_AWAIT accept_rpc<Remote, Local>(
            fixed_factory<Remote, Local>(std::move(local_interface)),
            settings,
            std::move(service),
            std::move(context),
            std::move(observe_transport));
    }
} // namespace rpc::connection_factory
