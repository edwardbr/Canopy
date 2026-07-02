/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

#include <connection_factory/context.h>
#include <connection_factory/options.h>

namespace rpc::connection_factory
{
    namespace detail
    {
        using settings_value = json::v1::object;
        using factory_context_value = context;
        using service_ptr = std::shared_ptr<rpc::service>;

        class stream_layer_builder
        {
        public:
            using function_type = std::function<CORO_TASK(stream_result)(
                std::shared_ptr<::streaming::stream>, settings_value, layer_direction, factory_context_value)>;

            stream_layer_builder() = default;
            stream_layer_builder(function_type function)
                : function_(std::move(function))
            {
            }

            explicit operator bool() const { return static_cast<bool>(function_); }

            CORO_TASK(stream_result)
            operator()(
                std::shared_ptr<::streaming::stream> stream,
                settings_value settings,
                layer_direction direction,
                factory_context_value factory_context) const
            {
                return function_(std::move(stream), std::move(settings), direction, std::move(factory_context));
            }

        private:
            function_type function_;
        };

        class base_stream_connect_builder
        {
        public:
            using function_type
                = std::function<CORO_TASK(stream_result)(settings_value, service_ptr, factory_context_value)>;

            base_stream_connect_builder() = default;
            base_stream_connect_builder(function_type function)
                : function_(std::move(function))
            {
            }

            explicit operator bool() const { return static_cast<bool>(function_); }

            CORO_TASK(stream_result)
            operator()(
                settings_value settings,
                service_ptr service,
                factory_context_value factory_context) const
            {
                return function_(std::move(settings), std::move(service), std::move(factory_context));
            }

        private:
            function_type function_;
        };

        class base_stream_acceptor_builder
        {
        public:
            using function_type
                = std::function<CORO_TASK(stream_acceptor_result)(settings_value, service_ptr, factory_context_value)>;

            base_stream_acceptor_builder() = default;
            base_stream_acceptor_builder(function_type function)
                : function_(std::move(function))
            {
            }

            explicit operator bool() const { return static_cast<bool>(function_); }

            CORO_TASK(stream_acceptor_result)
            operator()(
                settings_value settings,
                service_ptr service,
                factory_context_value factory_context) const
            {
                return function_(std::move(settings), std::move(service), std::move(factory_context));
            }

        private:
            function_type function_;
        };

        class base_stream_accept_builder
        {
        public:
            using function_type
                = std::function<CORO_TASK(stream_result)(settings_value, service_ptr, factory_context_value)>;

            base_stream_accept_builder() = default;
            base_stream_accept_builder(function_type function)
                : function_(std::move(function))
            {
            }

            explicit operator bool() const { return static_cast<bool>(function_); }

            CORO_TASK(stream_result)
            operator()(
                settings_value settings,
                service_ptr service,
                factory_context_value factory_context) const
            {
                return function_(std::move(settings), std::move(service), std::move(factory_context));
            }

        private:
            function_type function_;
        };

        template<class Settings, class = void> struct has_generated_settings_schema : std::false_type
        {
        };

        template<class Settings>
        struct has_generated_settings_schema<Settings, std::void_t<decltype(Settings::get_schema(rpc::encoding::yas_json))>>
            : std::true_type
        {
        };

        template<class Settings>
        constexpr bool has_generated_settings_schema_v = has_generated_settings_schema<Settings>::value;

        template<class Settings> constexpr void validate_typed_registration_settings()
        {
            static_assert(
                has_generated_settings_schema_v<Settings>,
                "connection_factory typed registration requires a generated IDL settings type with "
                "static get_schema(rpc::encoding)");
            static_assert(
                std::is_default_constructible_v<Settings>,
                "connection_factory typed registration requires default-constructible generated settings");
        }

        template<class Settings> struct typed_base_stream_connect_adapter
        {
            typed_base_stream_connect_builder<Settings> builder;

            auto operator()(
                json::v1::object settings,
                std::shared_ptr<rpc::service> service,
                context factory_context) const -> CORO_TASK(stream_result)
            {
                auto typed = materialise_settings<Settings>(settings);
                if (typed.error_code != rpc::error::OK())
                    CO_RETURN stream_result{typed.error_code, {}};
                CO_RETURN CO_AWAIT builder(std::move(typed.settings), std::move(service), factory_context);
            }
        };

        template<class Settings> struct typed_base_stream_acceptor_adapter
        {
            typed_base_stream_acceptor_builder<Settings> builder;

            auto operator()(
                json::v1::object settings,
                std::shared_ptr<rpc::service> service,
                context factory_context) const -> CORO_TASK(stream_acceptor_result)
            {
                auto typed = materialise_settings<Settings>(settings);
                if (typed.error_code != rpc::error::OK())
                    CO_RETURN stream_acceptor_result{typed.error_code, {}, {}, 0};
                CO_RETURN CO_AWAIT builder(std::move(typed.settings), std::move(service), factory_context);
            }
        };

        template<class Settings> struct typed_base_stream_accept_adapter
        {
            typed_base_stream_accept_builder<Settings> builder;

            auto operator()(
                json::v1::object settings,
                std::shared_ptr<rpc::service> service,
                context factory_context) const -> CORO_TASK(stream_result)
            {
                auto typed = materialise_settings<Settings>(settings);
                if (typed.error_code != rpc::error::OK())
                    CO_RETURN stream_result{typed.error_code, {}};
                CO_RETURN CO_AWAIT builder(std::move(typed.settings), std::move(service), factory_context);
            }
        };

        template<class Settings> struct typed_stream_layer_adapter
        {
            typed_stream_layer_builder<Settings> builder;

            auto operator()(
                std::shared_ptr<::streaming::stream> stream,
                json::v1::object settings,
                layer_direction direction,
                context factory_context) const -> CORO_TASK(stream_result)
            {
                auto typed = materialise_settings<Settings>(settings);
                if (typed.error_code != rpc::error::OK())
                    CO_RETURN stream_result{typed.error_code, std::move(stream)};
                CO_RETURN CO_AWAIT builder(std::move(stream), std::move(typed.settings), direction, factory_context);
            }
        };
    } // namespace detail

    template<class Settings>
    void context::register_connect_base_stream(
        std::string type,
        typed_base_stream_connect_builder<Settings> builder)
    {
        detail::validate_typed_registration_settings<Settings>();
        register_connect_base_stream_impl(
            std::move(type),
            detail::base_stream_connect_builder{detail::typed_base_stream_connect_adapter<Settings>{std::move(builder)}});
    }

    template<class Settings>
    void context::register_accept_base_stream(
        std::string type,
        typed_base_stream_acceptor_builder<Settings> builder)
    {
        detail::validate_typed_registration_settings<Settings>();
        register_accept_base_stream_impl(
            std::move(type),
            detail::base_stream_acceptor_builder{detail::typed_base_stream_acceptor_adapter<Settings>{std::move(builder)}});
    }

    template<class Settings>
    void context::register_accept_single_stream(
        std::string type,
        typed_base_stream_accept_builder<Settings> builder)
    {
        detail::validate_typed_registration_settings<Settings>();
        register_accept_single_stream_impl(
            std::move(type),
            detail::base_stream_accept_builder{detail::typed_base_stream_accept_adapter<Settings>{std::move(builder)}});
    }

    template<class Settings>
    void context::register_stream_layer(
        std::string type,
        typed_stream_layer_builder<Settings> builder)
    {
        detail::validate_typed_registration_settings<Settings>();
        register_stream_layer_impl(
            std::move(type),
            detail::stream_layer_builder{detail::typed_stream_layer_adapter<Settings>{std::move(builder)}});
    }
} // namespace rpc::connection_factory
