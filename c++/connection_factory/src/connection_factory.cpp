/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <connection_factory/connection_factory.h>

#include <exception>
#include <new>
#include <typeindex>
#include <utility>

#include <connection_factory/detail/context.h>
#include <connection_factory/detail/service.h>
#include <connection_factory_components.h>
#include <streaming/layer_factory/factory.h>
#include <streaming/stream.h>

#ifdef CANOPY_CONNECTION_FACTORY_HAS_SPSC
#  include <streaming/spsc_queue/factory.h>
#endif

#ifdef CANOPY_CONNECTION_FACTORY_HAS_TLS
#  include <streaming/secure_stream.h>
#endif

#ifdef CANOPY_CONNECTION_FACTORY_HAS_ATTESTATION
#  include <security/attestation/service.h>
#endif

#ifdef CANOPY_CONNECTION_FACTORY_HAS_LOCAL
#  include <transports/local/transport.h>
#endif

#ifdef CANOPY_CONNECTION_FACTORY_HAS_SGX_COROUTINE
#  include <io_uring/host_io_uring.h>
#  include <transports/sgx_coroutine/host/rpc_bootstrap.h>
#  include <transports/sgx_coroutine/host/connect.h>
#  include <transports/sgx_coroutine/host/transport.h>
#endif

#ifdef CANOPY_CONNECTION_FACTORY_HAS_SGX_BLOCKING
#  include <transports/sgx_blocking/transport.h>
#endif

namespace rpc::connection_factory
{
    struct context::impl
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

    namespace detail
    {
        class connection_factory_access
        {
        public:
            static auto state(context& factory_context) -> std::shared_ptr<context::impl>&
            {
                return factory_context.impl_;
            }

            static auto state(const context& factory_context) -> const std::shared_ptr<context::impl>&
            {
                return factory_context.impl_;
            }
        };

        [[nodiscard]] int validate_stream_rpc_connection_settings(
            const connection_settings& settings,
            layer_direction direction,
            const context& factory_context);

        auto connect_base_stream(
            const rpc::stream_layers::stream_layer_settings& layer,
            std::shared_ptr<rpc::service> service,
            const context& factory_context) -> CORO_TASK(stream_result);

        auto accept_base_streams(
            const rpc::stream_layers::stream_layer_settings& layer,
            std::shared_ptr<rpc::service> service,
            const context& factory_context) -> CORO_TASK(stream_acceptor_result);

        auto accept_single_base_stream(
            const rpc::stream_layers::stream_layer_settings& layer,
            std::shared_ptr<rpc::service> service,
            const context& factory_context) -> CORO_TASK(stream_result);

        auto apply_stream_layer(
            std::shared_ptr<::streaming::stream> stream,
            const rpc::stream_layers::stream_layer_settings& layer,
            layer_direction direction,
            const context& factory_context) -> CORO_TASK(stream_result);

        auto apply_stream_layers(
            std::shared_ptr<::streaming::stream> stream,
            const connection_settings& settings,
            size_t first_layer,
            layer_direction direction,
            const context& factory_context) -> CORO_TASK(stream_result);
    } // namespace detail

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

        bool built_in_stream_layer_supported(const std::string& type)
        {
#ifdef CANOPY_CONNECTION_FACTORY_HAS_WEBSOCKET
            if (type == "websocket")
                return true;
#endif

#ifdef CANOPY_CONNECTION_FACTORY_HAS_TLS
            if (type == "tls")
                return true;
#endif

#ifdef CANOPY_CONNECTION_FACTORY_HAS_SPSC_WRAPPING
            if (type == "spsc_wrapping" || type == "spsc_wrapper")
                return true;
#endif

#ifdef CANOPY_CONNECTION_FACTORY_HAS_ATTESTATION
            if (type == "attestation" || type == "attestation_stream")
                return true;
#endif

            return false;
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

    } // namespace

    const context& default_context()
    {
        static const context factory_context;
        return factory_context;
    }

    context::context()
        : impl_(std::make_shared<impl>())
    {
    }

    context::~context() = default;

    void context::set_dependency_impl(
        std::type_index type,
        std::string name,
        std::shared_ptr<void> dependency)
    {
        if (!impl_)
            impl_ = std::make_shared<impl>();
        impl_->set_dependency(type, std::move(name), std::move(dependency));
    }

    auto context::get_dependency_impl(
        std::type_index type,
        const std::string& name) const -> std::shared_ptr<void>
    {
        if (!impl_)
            return {};
        return impl_->get_dependency(type, name);
    }

    auto context::get_dependencies_impl(std::type_index type) const -> std::unordered_map<
        std::string,
        std::shared_ptr<void>>
    {
        if (!impl_)
            return {};
        return impl_->get_dependencies(type);
    }

#ifdef CANOPY_CONNECTION_FACTORY_HAS_SPSC
    void context::set_spsc_queues(rpc::spsc_queue::queue_pair queues)
    {
        set_dependency_value(std::move(queues));
    }
#endif

#ifdef CANOPY_CONNECTION_FACTORY_HAS_TLS
    void context::set_tls_client_context(std::shared_ptr<::streaming::secure::client_context> tls_context)
    {
        set_dependency(std::move(tls_context));
    }

    void context::set_tls_server_context(std::shared_ptr<::streaming::secure::context> tls_context)
    {
        set_dependency(std::move(tls_context));
    }
#endif

#ifdef CANOPY_CONNECTION_FACTORY_HAS_SPSC_WRAPPING
    void context::set_stream_scheduler(std::shared_ptr<rpc::executor> executor)
    {
        set_dependency(std::move(executor));
    }
#endif

#ifdef CANOPY_CONNECTION_FACTORY_HAS_ATTESTATION
    void context::set_attestation_service(std::shared_ptr<canopy::security::attestation::attestation_service> service)
    {
        set_dependency(std::move(service));
    }

    void context::register_attestation_service(
        std::string name,
        std::shared_ptr<canopy::security::attestation::attestation_service> service)
    {
        set_dependency(std::move(service), std::move(name));
    }
#endif

    void context::register_connect_base_stream(
        std::string type,
        base_stream_connect_builder builder)
    {
        if (!impl_)
            impl_ = std::make_shared<impl>();
        custom_stream_component(*impl_, type).connect_base_builder = std::move(builder);
    }

    void context::register_accept_base_stream(
        std::string type,
        base_stream_acceptor_builder builder)
    {
        if (!impl_)
            impl_ = std::make_shared<impl>();
        custom_stream_component(*impl_, type).accept_base_builder = std::move(builder);
    }

    void context::register_accept_single_stream(
        std::string type,
        base_stream_accept_builder builder)
    {
        if (!impl_)
            impl_ = std::make_shared<impl>();
        custom_stream_component(*impl_, type).accept_single_base_builder = std::move(builder);
    }

    void context::register_stream_layer(
        std::string type,
        stream_layer_builder builder)
    {
        if (!impl_)
            impl_ = std::make_shared<impl>();
        custom_stream_component(*impl_, type).stream_layer_builder_fn = std::move(builder);
    }

    int detail::validate_stream_rpc_connection_settings(
        const connection_settings& settings,
        layer_direction direction,
        const layered_connection_context& context)
    {
        if (settings.stream_layers.empty())
            return rpc::error::INVALID_DATA();

        const auto component_supports_base = [&](const std::string& type)
        {
            if (type.empty())
                return false;

            const auto& context_state = detail::connection_factory_access::state(context);
            if (context_state)
            {
                const auto custom = context_state->custom_stream_components.find(type);
                if (custom != context_state->custom_stream_components.end())
                {
                    if (direction == layer_direction::connect && custom->second->supports_connect_base())
                        return true;
                    if (direction == layer_direction::accept
                        && (custom->second->supports_accept_base() || custom->second->supports_accept_single_base()))
                    {
                        return true;
                    }
                }
            }

            const auto* built_in = built_in_stream_component(type);
            if (!built_in)
                return false;
            if (direction == layer_direction::connect)
                return built_in->supports_connect_base();
            return built_in->supports_accept_base() || built_in->supports_accept_single_base();
        };

        if (!component_supports_base(settings.stream_layers.front().type))
            return rpc::error::INVALID_DATA();

        for (size_t layer_index = 1; layer_index < settings.stream_layers.size(); ++layer_index)
        {
            const auto& layer = settings.stream_layers[layer_index];
            if (layer.type.empty())
                return rpc::error::INVALID_DATA();

            bool layer_supported = false;
            const auto& context_state = detail::connection_factory_access::state(context);
            if (context_state)
            {
                const auto custom = context_state->custom_stream_components.find(layer.type);
                layer_supported = custom != context_state->custom_stream_components.end()
                                  && custom->second->supports_stream_layer();
            }

            if (!layer_supported)
                layer_supported = built_in_stream_layer_supported(layer.type);

            if (!layer_supported)
                return rpc::error::INVALID_DATA();
        }

        return rpc::error::OK();
    }

    auto detail::make_transport_connect_context(
        const typed_settings& transport_settings,
        const connection_settings& settings,
        std::shared_ptr<rpc::service> service) -> transport_connect_context
    {
        const auto* built_in = built_in_transport_component(transport_settings.type);
        if (built_in)
        {
            return built_in->connect_transport(detail::settings_object(transport_settings), settings, std::move(service));
        }

        return {rpc::error::INVALID_DATA(), {}, {}, {}};
    }

    auto detail::make_native_transport_connect_context(
        const transport_selection_result& transport,
        const connection_settings& settings,
        std::shared_ptr<rpc::service> service) -> native_transport_connect_context
    {
        if (transport.error_code != rpc::error::OK())
            return {transport.error_code, {}, {}, {}};
        if (!transport.settings)
            return {rpc::error::INVALID_DATA(), {}, {}, {}};

        auto connect_context = make_transport_connect_context(*transport.settings, settings, std::move(service));
        if (connect_context.error_code != rpc::error::OK())
            return {connect_context.error_code, {}, {}, {}};
        if (!connect_context.service || !connect_context.transport)
            return {rpc::error::INVALID_DATA(), {}, {}, {}};

        return {rpc::error::OK(),
            std::move(connect_context.service),
            std::move(connect_context.transport),
            std::move(connect_context.service_proxy_name)};
    }

#ifdef CANOPY_CONNECTION_FACTORY_HAS_SGX_COROUTINE
    auto detail::make_sgx_coroutine_bootstrap_context(
        const transport_selection_result& transport,
        const connection_settings& settings,
        std::shared_ptr<rpc::service> service,
        const layered_connection_context& context,
        rpc::shared_ptr<rpc::i_noop> input_interface) -> rpc::sgx_coroutine_transport::host::rpc_bootstrap_context
    {
        if (!settings.stream_layers.empty())
            return {rpc::error::INVALID_DATA(), {}, {}, {}, {}};

        auto connect_context = make_native_transport_connect_context(transport, settings, std::move(service));
        if (connect_context.error_code != rpc::error::OK())
            return {connect_context.error_code, {}, {}, {}, {}};
        if (!connect_context.service || !connect_context.transport)
            return {rpc::error::INVALID_DATA(), {}, {}, {}, {}};

        auto enclave_transport
            = std::dynamic_pointer_cast<rpc::sgx_coroutine_transport::host::transport>(connect_context.transport);
        if (!enclave_transport)
            return {rpc::error::INVALID_DATA(), {}, {}, {}, {}};

        auto controller_options = rpc::io_uring::default_enclave_host_controller_options();
        if (auto configured_options = context.get_dependency<rpc::io_uring::host_controller::options>())
            controller_options = *configured_options;
        if (auto enclave_options = enclave_transport->get_enclave_io_uring_options())
            controller_options = *enclave_options;

        std::unique_ptr<rpc::io_uring::host_controller> controller;
        auto controller_error = rpc::io_uring::host_controller::create(
            controller, controller_options, connect_context.service->get_scheduler());
        if (controller_error != rpc::error::OK())
            return {controller_error, {}, {}, {}, {}};

        rpc::shared_ptr<rpc::v4::secure_coroutine_module::i_io_uring_control> control;
        try
        {
            control = rpc::shared_ptr<rpc::v4::secure_coroutine_module::i_io_uring_control>(
                new rpc::sgx_coroutine_transport::host::detail::enclave_io_uring_control(
                    std::move(controller), std::move(input_interface)));
        }
        catch (const std::bad_alloc&)
        {
            RPC_ERROR("bad_alloc while creating enclave io_uring control interface");
            std::terminate();
        }

        return {rpc::error::OK(),
            std::move(connect_context.service),
            std::move(enclave_transport),
            std::move(connect_context.service_proxy_name),
            std::move(control)};
    }
#endif

    auto detail::connect_base_stream(
        const rpc::stream_layers::stream_layer_settings& layer,
        std::shared_ptr<rpc::service> service,
        const layered_connection_context& context) -> CORO_TASK(stream_result)
    {
        auto& state = *detail::connection_factory_access::state(context);

        const auto custom = state.custom_stream_components.find(layer.type);
        if (custom != state.custom_stream_components.end() && custom->second->supports_connect_base())
        {
            CO_RETURN CO_AWAIT custom->second->connect_base(detail::settings_object(layer), std::move(service), context);
        }

        const auto* built_in = built_in_stream_component(layer.type);
        if (built_in && built_in->supports_connect_base())
        {
            CO_RETURN CO_AWAIT built_in->connect_base(detail::settings_object(layer), std::move(service), context);
        }

        CO_RETURN stream_result{rpc::error::INVALID_DATA(), {}};
    }

    auto detail::accept_base_streams(
        const rpc::stream_layers::stream_layer_settings& layer,
        std::shared_ptr<rpc::service> service,
        const layered_connection_context& context) -> CORO_TASK(stream_acceptor_result)
    {
        auto& state = *detail::connection_factory_access::state(context);

        const auto custom = state.custom_stream_components.find(layer.type);
        if (custom != state.custom_stream_components.end() && custom->second->supports_accept_base())
        {
            CO_RETURN CO_AWAIT custom->second->accept_base(detail::settings_object(layer), std::move(service), context);
        }

        const auto* built_in = built_in_stream_component(layer.type);
        if (built_in && built_in->supports_accept_base())
        {
            CO_RETURN CO_AWAIT built_in->accept_base(detail::settings_object(layer), std::move(service), context);
        }

        CO_RETURN stream_acceptor_result{rpc::error::INVALID_DATA(), {}, {}, 0};
    }

    auto detail::accept_single_base_stream(
        const rpc::stream_layers::stream_layer_settings& layer,
        std::shared_ptr<rpc::service> service,
        const layered_connection_context& context) -> CORO_TASK(stream_result)
    {
        auto& state = *detail::connection_factory_access::state(context);

        const auto custom = state.custom_stream_components.find(layer.type);
        if (custom != state.custom_stream_components.end() && custom->second->supports_accept_single_base())
        {
            CO_RETURN CO_AWAIT custom->second->accept_single_base(
                detail::settings_object(layer), std::move(service), context);
        }

        const auto* built_in = built_in_stream_component(layer.type);
        if (built_in && built_in->supports_accept_single_base())
        {
            CO_RETURN CO_AWAIT built_in->accept_single_base(detail::settings_object(layer), std::move(service), context);
        }

        CO_RETURN stream_result{rpc::error::INVALID_DATA(), {}};
    }

    auto detail::apply_stream_layer(
        std::shared_ptr<::streaming::stream> stream,
        const rpc::stream_layers::stream_layer_settings& layer,
        layer_direction direction,
        const layered_connection_context& context) -> CORO_TASK(stream_result)
    {
        auto& state = *detail::connection_factory_access::state(context);

        const auto custom = state.custom_stream_components.find(layer.type);
        if (custom != state.custom_stream_components.end() && custom->second->supports_stream_layer())
        {
            CO_RETURN CO_AWAIT custom->second->wrap_stream(
                std::move(stream), detail::settings_object(layer), direction, context);
        }

        auto layer_context = make_streaming_layer_context(state, layer.type);
        auto wrapped = CO_AWAIT ::streaming::layer_factory::apply_stream_layer_async(
            std::move(stream), layer, layer_factory_direction(direction), layer_context);
        CO_RETURN stream_result{wrapped.error_code, std::move(wrapped.stream)};
    }

    auto detail::apply_stream_layers(
        std::shared_ptr<::streaming::stream> stream,
        const connection_settings& settings,
        size_t first_layer,
        layer_direction direction,
        const layered_connection_context& context) -> CORO_TASK(stream_result)
    {
        if (first_layer > settings.stream_layers.size())
            CO_RETURN stream_result{rpc::error::INVALID_DATA(), {}};

        for (auto layer_index = first_layer; layer_index < settings.stream_layers.size(); ++layer_index)
        {
            auto result = CO_AWAIT detail::apply_stream_layer(
                std::move(stream), settings.stream_layers[layer_index], direction, context);
            if (result.error_code != rpc::error::OK())
                CO_RETURN result;
            stream = std::move(result.stream);
        }

        CO_RETURN stream_result{rpc::error::OK(), std::move(stream)};
    }

    CORO_TASK(stream_acceptor_result)
    open_stream_acceptor(
        const connection_settings& settings,
        std::shared_ptr<rpc::service> service,
        const context& factory_context)
    {
        auto topology_error
            = detail::validate_stream_rpc_connection_settings(settings, layer_direction::accept, factory_context);
        if (topology_error != rpc::error::OK())
            CO_RETURN stream_acceptor_result{topology_error, {}, {}, 0};

        CO_RETURN CO_AWAIT detail::accept_base_streams(settings.stream_layers.front(), std::move(service), factory_context);
    }

    auto detail::make_accept_stream_transformer(
        const connection_settings& settings,
        size_t first_layer,
        const context& factory_context) -> ::streaming::listener::stream_transformer
    {
        if (first_layer >= settings.stream_layers.size())
            return {};

        return [settings, factory_context, first_layer](std::shared_ptr<::streaming::stream> stream)
                   -> CORO_TASK(std::optional<std::shared_ptr<::streaming::stream>>)
        {
            auto wrapped = CO_AWAIT detail::apply_stream_layers(
                std::move(stream), settings, first_layer, layer_direction::accept, factory_context);
            if (wrapped.error_code != rpc::error::OK())
                CO_RETURN std::nullopt;
            CO_RETURN std::optional<std::shared_ptr<::streaming::stream>>(std::move(wrapped.stream));
        };
    }

    CORO_TASK(stream_result)
    connect_stream(
        const connection_settings& settings,
        std::shared_ptr<rpc::service> service,
        const context& factory_context)
    {
        auto topology_error
            = detail::validate_stream_rpc_connection_settings(settings, layer_direction::connect, factory_context);
        if (topology_error != rpc::error::OK())
            CO_RETURN stream_result{topology_error, {}};

        auto service_settings = detail::service_settings_from_connection(settings);
        if (service_settings.error_code != rpc::error::OK())
            CO_RETURN stream_result{service_settings.error_code, {}};

        auto resolved_service = ensure_service(service_settings.settings, {}, std::move(service), "layered_stream_client");
        if (!resolved_service)
            CO_RETURN stream_result{rpc::error::INVALID_DATA(), {}};

        auto stream
            = CO_AWAIT detail::connect_base_stream(settings.stream_layers.front(), resolved_service, factory_context);
        if (stream.error_code != rpc::error::OK())
            CO_RETURN stream;

        CO_RETURN CO_AWAIT detail::apply_stream_layers(
            std::move(stream.stream), settings, 1, layer_direction::connect, factory_context);
    }

    CORO_TASK(stream_result)
    accept_stream(
        const connection_settings& settings,
        std::shared_ptr<rpc::service> service,
        const context& factory_context)
    {
        auto topology_error
            = detail::validate_stream_rpc_connection_settings(settings, layer_direction::accept, factory_context);
        if (topology_error != rpc::error::OK())
            CO_RETURN stream_result{topology_error, {}};

        auto service_settings = detail::service_settings_from_connection(settings);
        if (service_settings.error_code != rpc::error::OK())
            CO_RETURN stream_result{service_settings.error_code, {}};

        auto resolved_service = ensure_service(service_settings.settings, {}, std::move(service), "layered_stream_accept");
        if (!resolved_service)
            CO_RETURN stream_result{rpc::error::INVALID_DATA(), {}};

        auto accepted_stream = CO_AWAIT detail::accept_single_base_stream(
            settings.stream_layers.front(), resolved_service, factory_context);
        if (accepted_stream.error_code != rpc::error::OK())
            CO_RETURN accepted_stream;

        CO_RETURN CO_AWAIT detail::apply_stream_layers(
            std::move(accepted_stream.stream), settings, 1, layer_direction::accept, factory_context);
    }

    CORO_TASK(stream_accept_result)
    accept_streams(
        stream_callback callback,
        const connection_settings& settings,
        std::shared_ptr<rpc::service> service,
        const context& factory_context)
    {
        if (!callback)
            CO_RETURN stream_accept_result{rpc::error::INVALID_DATA(), {}};

        auto service_settings = detail::service_settings_from_connection(settings);
        if (service_settings.error_code != rpc::error::OK())
            CO_RETURN stream_accept_result{service_settings.error_code, {}};

        auto resolved_service = ensure_service(service_settings.settings, {}, std::move(service), "layered_stream_accept");
        if (!resolved_service)
            CO_RETURN stream_accept_result{rpc::error::INVALID_DATA(), {}};

        auto acceptor = CO_AWAIT open_stream_acceptor(settings, resolved_service, factory_context);
        if (acceptor.error_code != rpc::error::OK())
            CO_RETURN stream_accept_result{acceptor.error_code, {}};

        auto stream_settings = make_stream_rpc_settings({}, service_settings.settings);
        auto wrapped_callback = [settings, factory_context, callback = std::move(callback)](
                                    std::shared_ptr<::streaming::stream> stream) mutable -> CORO_TASK(void)
        {
            auto wrapped = CO_AWAIT detail::apply_stream_layers(
                std::move(stream), settings, 1, layer_direction::accept, factory_context);
            if (wrapped.error_code != rpc::error::OK())
            {
                if (wrapped.stream)
                    CO_AWAIT wrapped.stream->set_closed();
                CO_RETURN;
            }

            CO_AWAIT callback(std::move(wrapped.stream));
            CO_RETURN;
        };

        CO_RETURN accept_streams(
            std::move(acceptor.acceptor),
            std::move(wrapped_callback),
            stream_settings,
            std::move(resolved_service),
            std::move(acceptor.owner),
            acceptor.port);
    }
} // namespace rpc::connection_factory
