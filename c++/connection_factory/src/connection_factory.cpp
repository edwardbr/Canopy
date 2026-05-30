/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <connection_factory/connection_factory.h>

#include <typeindex>
#include <utility>

#include <connection_factory/service.h>
#include <connection_factory_components.h>
#include <streaming/layer_factory/factory.h>

#ifdef CANOPY_CONNECTION_FACTORY_HAS_LOCAL
#  include <transports/local/transport.h>
#endif

#ifdef CANOPY_CONNECTION_FACTORY_HAS_SGX_COROUTINE
#  include <transports/sgx_coroutine/host/transport.h>
#endif

#ifdef CANOPY_CONNECTION_FACTORY_HAS_SGX_BLOCKING
#  include <transports/sgx_blocking/transport.h>
#endif

namespace rpc::connection_factory
{
    struct layered_connection_context::impl
    {
        std::unordered_map<std::type_index, std::unordered_map<std::string, std::shared_ptr<void>>> dependencies;
        std::unordered_map<std::string, std::shared_ptr<detail::registered_stream_component_factory>> custom_stream_components;

        void set_dependency(
            std::type_index type,
            std::string name,
            std::shared_ptr<void> dependency)
        {
            auto& typed_dependencies = dependencies[type];
            if (dependency)
            {
                typed_dependencies[std::move(name)] = std::move(dependency);
                return;
            }

            typed_dependencies.erase(name);
            if (typed_dependencies.empty())
                dependencies.erase(type);
        }

        auto get_dependency(
            std::type_index type,
            const std::string& name) const -> std::shared_ptr<void>
        {
            const auto type_item = dependencies.find(type);
            if (type_item == dependencies.end())
                return {};
            const auto dependency_item = type_item->second.find(name);
            if (dependency_item == type_item->second.end())
                return {};
            return dependency_item->second;
        }

        auto get_dependencies(std::type_index type) const -> std::unordered_map<
            std::string,
            std::shared_ptr<void>>
        {
            const auto type_item = dependencies.find(type);
            if (type_item == dependencies.end())
                return {};
            return type_item->second;
        }
    };

    namespace
    {
        template<
            class T,
            class State>
        auto dependency_from_state(
            State& state,
            const std::string& name = {}) -> std::shared_ptr<T>
        {
            return std::static_pointer_cast<T>(state.get_dependency(std::type_index(typeid(T)), name));
        }

        template<
            class T,
            class State>
        void set_dependency_in_state(
            State& state,
            std::shared_ptr<T> dependency,
            std::string name = {})
        {
            state.set_dependency(
                std::type_index(typeid(T)), std::move(name), std::static_pointer_cast<void>(std::move(dependency)));
        }

        template<
            class T,
            class State>
        auto dependencies_from_state(State& state) -> std::unordered_map<
            std::string,
            std::shared_ptr<T>>
        {
            std::unordered_map<std::string, std::shared_ptr<T>> result;
            auto dependencies = state.get_dependencies(std::type_index(typeid(T)));
            for (auto& [name, dependency] : dependencies)
                result.emplace(std::move(name), std::static_pointer_cast<T>(std::move(dependency)));
            return result;
        }

        auto built_in_stream_components() -> const detail::stream_component_map&
        {
            static const auto components = []
            {
                detail::stream_component_map result;

#ifdef CANOPY_CONNECTION_FACTORY_HAS_TCP_BLOCKING
                detail::register_tcp_blocking_stream_components(result);
#endif

#ifdef CANOPY_CONNECTION_FACTORY_HAS_TCP_COROUTINE
                detail::register_tcp_coroutine_stream_components(result);
#endif

#ifdef CANOPY_CONNECTION_FACTORY_HAS_SPSC
                detail::register_spsc_queue_stream_components(result);
#endif

                return result;
            }();
            return components;
        }

        auto built_in_stream_component(const std::string& type) -> const detail::stream_component_factory*
        {
            const auto& components = built_in_stream_components();
            const auto found = components.find(type);
            if (found == components.end())
                return nullptr;
            return found->second.get();
        }

        auto built_in_transport_components() -> const detail::transport_component_map&
        {
            static const auto components = []
            {
                detail::transport_component_map result;

#ifdef CANOPY_CONNECTION_FACTORY_HAS_LOCAL
                detail::register_local_transport_components(result);
#endif

#ifdef CANOPY_CONNECTION_FACTORY_HAS_SGX_BLOCKING
                detail::register_sgx_blocking_transport_components(result);
#endif

#ifdef CANOPY_CONNECTION_FACTORY_HAS_SGX_COROUTINE
                detail::register_sgx_coroutine_transport_components(result);
#endif

                return result;
            }();
            return components;
        }

        auto built_in_transport_component(const std::string& type) -> const detail::transport_component_factory*
        {
            const auto& components = built_in_transport_components();
            const auto found = components.find(type);
            if (found == components.end())
                return nullptr;
            return found->second.get();
        }

        template<class State>
        auto custom_stream_component(
            State& state,
            const std::string& type) -> detail::registered_stream_component_factory&
        {
            auto& component = state.custom_stream_components[type];
            if (!component)
                component = std::make_shared<detail::registered_stream_component_factory>();
            return *component;
        }

        auto layer_factory_direction(layer_direction direction) -> ::streaming::layer_factory::layer_direction
        {
            if (direction == layer_direction::accept)
                return ::streaming::layer_factory::layer_direction::accept;
            return ::streaming::layer_factory::layer_direction::connect;
        }

        template<class State>
        auto make_streaming_layer_context(
            State& state,
            const std::string& layer_type) -> ::streaming::layer_factory::layer_context
        {
            ::streaming::layer_factory::layer_context result;

#if defined(CANOPY_CONNECTION_FACTORY_HAS_TLS) && defined(CANOPY_STREAMING_LAYER_FACTORY_HAS_TLS)
            result.tls_client_context = dependency_from_state<::streaming::secure::client_context>(state);
            result.tls_server_context = dependency_from_state<::streaming::secure::context>(state);
#endif

#if defined(CANOPY_CONNECTION_FACTORY_HAS_SPSC_WRAPPING) && defined(CANOPY_STREAMING_LAYER_FACTORY_HAS_SPSC_WRAPPING)
            if (layer_type == "spsc_wrapping" || layer_type == "spsc_wrapper")
            {
                auto scheduler = dependency_from_state<rpc::executor>(state);
                if (!scheduler)
                {
                    scheduler = make_default_executor();
                    set_dependency_in_state(state, scheduler);
                }
                result.stream_scheduler = std::move(scheduler);
            }
#else
            (void)layer_type;
#endif

#if defined(CANOPY_CONNECTION_FACTORY_HAS_ATTESTATION) && defined(CANOPY_STREAMING_LAYER_FACTORY_HAS_ATTESTATION)
            using attestation_service = canopy::security::attestation::attestation_service;
            result.attestation_service = dependency_from_state<attestation_service>(state);
            auto services = dependencies_from_state<attestation_service>(state);
            services.erase("");
            result.named_attestation_services = std::move(services);
#endif

            return result;
        }

#ifdef CANOPY_CONNECTION_FACTORY_HAS_SGX_COROUTINE
        int validate_sgx_stream_layers(const std::vector<rpc::stream_layers::stream_layer_settings>& layers)
        {
            for (const auto& layer : layers)
            {
#  ifdef CANOPY_CONNECTION_FACTORY_HAS_WEBSOCKET
                if (layer.type == "websocket")
                    continue;
#  endif
                return rpc::error::INVALID_DATA();
            }
            return rpc::error::OK();
        }
#endif
    } // namespace

    layered_connection_context::layered_connection_context()
        : impl_(std::make_shared<impl>())
    {
    }

    layered_connection_context::~layered_connection_context() = default;

    void layered_connection_context::set_dependency_impl(
        std::type_index type,
        std::string name,
        std::shared_ptr<void> dependency)
    {
        if (!impl_)
            impl_ = std::make_shared<impl>();
        impl_->set_dependency(type, std::move(name), std::move(dependency));
    }

    auto layered_connection_context::get_dependency_impl(
        std::type_index type,
        const std::string& name) const -> std::shared_ptr<void>
    {
        if (!impl_)
            return {};
        return impl_->get_dependency(type, name);
    }

    auto layered_connection_context::get_dependencies_impl(std::type_index type) const -> std::unordered_map<
        std::string,
        std::shared_ptr<void>>
    {
        if (!impl_)
            return {};
        return impl_->get_dependencies(type);
    }

#ifdef CANOPY_CONNECTION_FACTORY_HAS_SPSC
    void layered_connection_context::set_spsc_queues(rpc::spsc_queue::queue_pair queues)
    {
        set_dependency_value(std::move(queues));
    }
#endif

#ifdef CANOPY_CONNECTION_FACTORY_HAS_TLS
    void layered_connection_context::set_tls_client_context(std::shared_ptr<::streaming::secure::client_context> context)
    {
        set_dependency(std::move(context));
    }

    void layered_connection_context::set_tls_server_context(std::shared_ptr<::streaming::secure::context> context)
    {
        set_dependency(std::move(context));
    }
#endif

#ifdef CANOPY_CONNECTION_FACTORY_HAS_SPSC_WRAPPING
    void layered_connection_context::set_stream_scheduler(std::shared_ptr<rpc::executor> executor)
    {
        set_dependency(std::move(executor));
    }
#endif

#ifdef CANOPY_CONNECTION_FACTORY_HAS_ATTESTATION
    void layered_connection_context::set_attestation_service(
        std::shared_ptr<canopy::security::attestation::attestation_service> service)
    {
        set_dependency(std::move(service));
    }

    void layered_connection_context::register_attestation_service(
        std::string name,
        std::shared_ptr<canopy::security::attestation::attestation_service> service)
    {
        set_dependency(std::move(service), std::move(name));
    }
#endif

    void layered_connection_context::register_connect_base_stream(
        std::string type,
        base_stream_connect_builder builder)
    {
        if (!impl_)
            impl_ = std::make_shared<impl>();
        custom_stream_component(*impl_, type).connect_base_builder = std::move(builder);
    }

    void layered_connection_context::register_accept_base_stream(
        std::string type,
        base_stream_acceptor_builder builder)
    {
        if (!impl_)
            impl_ = std::make_shared<impl>();
        custom_stream_component(*impl_, type).accept_base_builder = std::move(builder);
    }

    void layered_connection_context::register_accept_single_stream(
        std::string type,
        base_stream_accept_builder builder)
    {
        if (!impl_)
            impl_ = std::make_shared<impl>();
        custom_stream_component(*impl_, type).accept_single_base_builder = std::move(builder);
    }

    void layered_connection_context::register_stream_layer(
        std::string type,
        stream_layer_builder builder)
    {
        if (!impl_)
            impl_ = std::make_shared<impl>();
        custom_stream_component(*impl_, type).stream_layer_builder_fn = std::move(builder);
    }

    auto detail::make_transport_connect_context(
        const rpc::connection_factory_config::typed_settings& transport_settings,
        const rpc::connection_factory_config::connection_settings& settings,
        std::shared_ptr<rpc::service> service) -> transport_connect_context
    {
        const auto* built_in = built_in_transport_component(transport_settings.type);
        if (built_in)
        {
            return built_in->connect_transport(settings_object(transport_settings), settings, std::move(service));
        }

        return {rpc::error::INVALID_DATA(), {}, {}, {}};
    }

#ifdef CANOPY_CONNECTION_FACTORY_HAS_LOCAL
    auto detail::make_local_child_connect_context(
        const rpc::connection_factory_config::connection_settings& settings,
        std::shared_ptr<rpc::service> service) -> local_child_connect_context
    {
        auto transport = transport_from_connection(settings);
        if (transport.error_code != rpc::error::OK())
            return {transport.error_code, {}, {}, {}};
        if (transport.type != "local" || !transport.settings)
            return {rpc::error::INVALID_DATA(), {}, {}, {}};
        if (!settings.stream_layers.empty())
            return {rpc::error::INVALID_DATA(), {}, {}, {}};

        auto local = make_transport_connect_context(*transport.settings, settings, std::move(service));
        if (local.error_code != rpc::error::OK())
            return {local.error_code, {}, {}, {}};
        if (!local.service || !local.transport)
            return {rpc::error::INVALID_DATA(), {}, {}, {}};

        auto local_transport = std::dynamic_pointer_cast<rpc::local::child_transport>(local.transport);
        if (!local_transport)
            return {rpc::error::INVALID_DATA(), {}, {}, {}};

        return {
            rpc::error::OK(), std::move(local.service), std::move(local_transport), std::move(local.service_proxy_name)};
    }
#endif

#ifdef CANOPY_CONNECTION_FACTORY_HAS_SGX_COROUTINE
    auto detail::make_sgx_coroutine_connect_context(
        const rpc::connection_factory_config::typed_settings& transport_settings,
        const rpc::connection_factory_config::connection_settings& settings,
        std::shared_ptr<rpc::service> service,
        const layered_connection_context& context) -> sgx_coroutine_connect_context
    {
        auto layer_validation = validate_sgx_stream_layers(settings.stream_layers);
        if (layer_validation != rpc::error::OK())
            return {layer_validation, {}, {}, {}, {}};

        auto connect_context = make_transport_connect_context(transport_settings, settings, std::move(service));
        if (connect_context.error_code != rpc::error::OK())
            return {connect_context.error_code, {}, {}, {}, {}};
        if (!connect_context.service || !connect_context.transport)
            return {rpc::error::INVALID_DATA(), {}, {}, {}, {}};

        auto enclave_transport
            = std::dynamic_pointer_cast<rpc::sgx_coroutine_transport::host::transport>(connect_context.transport);
        if (!enclave_transport)
            return {rpc::error::INVALID_DATA(), {}, {}, {}, {}};

        if (!settings.stream_layers.empty())
        {
            auto layer_error = enclave_transport->set_enclave_stream_layers(settings.stream_layers);
            if (layer_error != rpc::error::OK())
                return {layer_error, {}, {}, {}, {}};

            enclave_transport->set_host_stream_layer_applier(
                [settings, context](std::shared_ptr<::streaming::stream> stream)
                    -> CORO_TASK(rpc::sgx_coroutine_transport::host::transport::stream_layer_result)
                {
                    auto wrapped = CO_AWAIT rpc::connection_factory::apply_stream_layers(
                        std::move(stream), settings, 0, rpc::connection_factory::layer_direction::connect, context);
                    CO_RETURN rpc::sgx_coroutine_transport::host::transport::stream_layer_result{
                        wrapped.error_code, std::move(wrapped.stream)};
                });
        }

        rpc::io_uring::host_controller::options controller_options;
        if (auto configured_options = context.get_dependency<rpc::io_uring::host_controller::options>())
            controller_options = *configured_options;
        if (auto enclave_options = enclave_transport->get_enclave_io_uring_options())
            controller_options = *enclave_options;

        return {rpc::error::OK(),
            std::move(connect_context.service),
            std::move(enclave_transport),
            std::move(connect_context.service_proxy_name),
            std::move(controller_options)};
    }
#endif

#ifdef CANOPY_CONNECTION_FACTORY_HAS_SGX_BLOCKING
    auto detail::make_sgx_blocking_connect_context(
        const rpc::connection_factory_config::typed_settings& transport_settings,
        const rpc::connection_factory_config::connection_settings& settings,
        std::shared_ptr<rpc::service> service) -> native_transport_connect_context
    {
        if (!settings.stream_layers.empty())
            return {rpc::error::INVALID_DATA(), {}, {}, {}};

        auto connect_context = make_transport_connect_context(transport_settings, settings, std::move(service));
        if (connect_context.error_code != rpc::error::OK())
            return {connect_context.error_code, {}, {}, {}};
        if (!connect_context.service || !connect_context.transport)
            return {rpc::error::INVALID_DATA(), {}, {}, {}};

        auto enclave_transport
            = std::dynamic_pointer_cast<rpc::sgx_blocking_transport::enclave_transport>(connect_context.transport);
        if (!enclave_transport)
            return {rpc::error::INVALID_DATA(), {}, {}, {}};

        return {rpc::error::OK(),
            std::move(connect_context.service),
            std::move(enclave_transport),
            std::move(connect_context.service_proxy_name)};
    }
#endif

    auto connect_base_stream(
        const rpc::stream_layers::stream_layer_settings& layer,
        std::shared_ptr<rpc::service> service,
        const layered_connection_context& context) -> CORO_TASK(stream_result)
    {
        auto& state = *context.impl_;

        const auto custom = state.custom_stream_components.find(layer.type);
        if (custom != state.custom_stream_components.end() && custom->second->supports_connect_base())
        {
            CO_RETURN CO_AWAIT custom->second->connect_base(settings_object(layer), std::move(service), context);
        }

        const auto* built_in = built_in_stream_component(layer.type);
        if (built_in && built_in->supports_connect_base())
        {
            CO_RETURN CO_AWAIT built_in->connect_base(settings_object(layer), std::move(service), context);
        }

        CO_RETURN stream_result{rpc::error::INVALID_DATA(), {}};
    }

    auto accept_base_streams(
        const rpc::stream_layers::stream_layer_settings& layer,
        std::shared_ptr<rpc::service> service,
        const layered_connection_context& context) -> CORO_TASK(stream_acceptor_result)
    {
        auto& state = *context.impl_;

        const auto custom = state.custom_stream_components.find(layer.type);
        if (custom != state.custom_stream_components.end() && custom->second->supports_accept_base())
        {
            CO_RETURN CO_AWAIT custom->second->accept_base(settings_object(layer), std::move(service), context);
        }

        const auto* built_in = built_in_stream_component(layer.type);
        if (built_in && built_in->supports_accept_base())
        {
            CO_RETURN CO_AWAIT built_in->accept_base(settings_object(layer), std::move(service), context);
        }

        CO_RETURN stream_acceptor_result{rpc::error::INVALID_DATA(), {}, {}, 0};
    }

    auto accept_single_base_stream(
        const rpc::stream_layers::stream_layer_settings& layer,
        std::shared_ptr<rpc::service> service,
        const layered_connection_context& context) -> CORO_TASK(stream_result)
    {
        auto& state = *context.impl_;

        const auto custom = state.custom_stream_components.find(layer.type);
        if (custom != state.custom_stream_components.end() && custom->second->supports_accept_single_base())
        {
            CO_RETURN CO_AWAIT custom->second->accept_single_base(settings_object(layer), std::move(service), context);
        }

        const auto* built_in = built_in_stream_component(layer.type);
        if (built_in && built_in->supports_accept_single_base())
        {
            CO_RETURN CO_AWAIT built_in->accept_single_base(settings_object(layer), std::move(service), context);
        }

        CO_RETURN stream_result{rpc::error::INVALID_DATA(), {}};
    }

    auto apply_stream_layer(
        std::shared_ptr<::streaming::stream> stream,
        const rpc::stream_layers::stream_layer_settings& layer,
        layer_direction direction,
        const layered_connection_context& context) -> CORO_TASK(stream_result)
    {
        auto& state = *context.impl_;

        const auto custom = state.custom_stream_components.find(layer.type);
        if (custom != state.custom_stream_components.end() && custom->second->supports_stream_layer())
        {
            CO_RETURN CO_AWAIT custom->second->wrap_stream(std::move(stream), settings_object(layer), direction, context);
        }

        auto layer_context = make_streaming_layer_context(state, layer.type);
        auto wrapped = CO_AWAIT ::streaming::layer_factory::apply_stream_layer_async(
            std::move(stream), layer, layer_factory_direction(direction), layer_context);
        CO_RETURN stream_result{wrapped.error_code, std::move(wrapped.stream)};
    }

    auto apply_stream_layers(
        std::shared_ptr<::streaming::stream> stream,
        const rpc::connection_factory_config::connection_settings& settings,
        size_t first_layer,
        layer_direction direction,
        const layered_connection_context& context) -> CORO_TASK(stream_result)
    {
        for (auto layer_index = first_layer; layer_index < settings.stream_layers.size(); ++layer_index)
        {
            auto result
                = CO_AWAIT apply_stream_layer(std::move(stream), settings.stream_layers[layer_index], direction, context);
            if (result.error_code != rpc::error::OK())
                CO_RETURN result;
            stream = std::move(result.stream);
        }

        CO_RETURN stream_result{rpc::error::OK(), std::move(stream)};
    }
} // namespace rpc::connection_factory
