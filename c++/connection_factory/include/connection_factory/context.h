/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <typeindex>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include <connection_factory/handles.h>

#ifdef CANOPY_CONNECTION_FACTORY_HAS_TLS
#  include <streaming/secure_stream.h>
#endif

namespace streaming
{
    class stream;
}

namespace rpc::spsc_queue
{
    struct queue_pair;
}

namespace canopy::security::attestation
{
    class attestation_service;
}

namespace rpc::connection_factory
{
    enum class layer_direction
    {
        connect,
        accept,
    };

    class context;

    namespace detail
    {
        class stream_layer_builder;
        class base_stream_connect_builder;
        class base_stream_acceptor_builder;
        class base_stream_accept_builder;
        class connection_factory_access;
    } // namespace detail

    template<class Settings>
    using typed_stream_layer_builder
        = std::function<CORO_TASK(stream_result)(std::shared_ptr<::streaming::stream>, Settings, layer_direction, const context&)>;

    template<class Settings>
    using typed_base_stream_connect_builder
        = std::function<CORO_TASK(stream_result)(Settings, std::shared_ptr<rpc::service>, const context&)>;

    template<class Settings>
    using typed_base_stream_acceptor_builder
        = std::function<CORO_TASK(stream_acceptor_result)(Settings, std::shared_ptr<rpc::service>, const context&)>;

    template<class Settings>
    using typed_base_stream_accept_builder
        = std::function<CORO_TASK(stream_result)(Settings, std::shared_ptr<rpc::service>, const context&)>;

    // Extension context for built-in dependencies and custom stream factories.
    // The normal connection_factory.h API uses default_context().
    class context
    {
    public:
        context();
        ~context();

        context(const context&) = default;
        context(context&&) noexcept = default;
        auto operator=(const context&) -> context& = default;
        auto operator=(context&&) noexcept -> context& = default;

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
        void set_tls_client_context(std::shared_ptr<::streaming::secure::client_context> tls_context);
        void set_tls_server_context(std::shared_ptr<::streaming::secure::context> tls_context);
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

        template<class Settings>
        void register_connect_base_stream(
            std::string type,
            typed_base_stream_connect_builder<Settings> builder);

        template<class Settings>
        void register_accept_base_stream(
            std::string type,
            typed_base_stream_acceptor_builder<Settings> builder);

        template<class Settings>
        void register_accept_single_stream(
            std::string type,
            typed_base_stream_accept_builder<Settings> builder);

        template<class Settings>
        void register_stream_layer(
            std::string type,
            typed_stream_layer_builder<Settings> builder);

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
        void register_connect_base_stream_impl(
            std::string type,
            detail::base_stream_connect_builder builder);
        void register_accept_base_stream_impl(
            std::string type,
            detail::base_stream_acceptor_builder builder);
        void register_accept_single_stream_impl(
            std::string type,
            detail::base_stream_accept_builder builder);
        void register_stream_layer_impl(
            std::string type,
            detail::stream_layer_builder builder);

        friend class detail::connection_factory_access;
    };

} // namespace rpc::connection_factory

#include <connection_factory/detail/context_registration.h>
