/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

#include <connection_factory/detail/context.h>
#include <connection_factory/detail/service.h>

namespace rpc::connection_factory::detail
{
    struct transport_connect_context
    {
        int error_code{rpc::error::OK()};
        std::shared_ptr<rpc::service> service;
        std::shared_ptr<rpc::transport> transport;
        std::string service_proxy_name;
    };

    class stream_component_factory
    {
    public:
        virtual ~stream_component_factory() = default;

        virtual bool supports_connect_base() const { return false; }
        virtual bool supports_accept_base() const { return false; }
        virtual bool supports_accept_single_base() const { return false; }
        virtual bool supports_stream_layer() const { return false; }

        virtual auto connect_base(
            const json::v1::object&,
            std::shared_ptr<rpc::service>,
            const layered_connection_context&) const -> CORO_TASK(stream_result)
        {
            CO_RETURN stream_result{rpc::error::INVALID_DATA(), {}};
        }

        virtual auto accept_base(
            const json::v1::object&,
            std::shared_ptr<rpc::service>,
            const layered_connection_context&) const -> CORO_TASK(stream_acceptor_result)
        {
            CO_RETURN stream_acceptor_result{rpc::error::INVALID_DATA(), {}, {}, 0};
        }

        virtual auto accept_single_base(
            const json::v1::object&,
            std::shared_ptr<rpc::service>,
            const layered_connection_context&) const -> CORO_TASK(stream_result)
        {
            CO_RETURN stream_result{rpc::error::INVALID_DATA(), {}};
        }

        virtual auto wrap_stream(
            std::shared_ptr<::streaming::stream>,
            const json::v1::object&,
            layer_direction,
            const layered_connection_context&) const -> CORO_TASK(stream_result)
        {
            CO_RETURN stream_result{rpc::error::INVALID_DATA(), {}};
        }
    };

    class registered_stream_component_factory final : public stream_component_factory
    {
    public:
        base_stream_connect_builder connect_base_builder;
        base_stream_acceptor_builder accept_base_builder;
        base_stream_accept_builder accept_single_base_builder;
        stream_layer_builder stream_layer_builder_fn;

        bool supports_connect_base() const override { return static_cast<bool>(connect_base_builder); }
        bool supports_accept_base() const override { return static_cast<bool>(accept_base_builder); }
        bool supports_accept_single_base() const override { return static_cast<bool>(accept_single_base_builder); }
        bool supports_stream_layer() const override { return static_cast<bool>(stream_layer_builder_fn); }

        auto connect_base(
            const json::v1::object& settings,
            std::shared_ptr<rpc::service> service,
            const layered_connection_context& context) const -> CORO_TASK(stream_result) override
        {
            CO_RETURN CO_AWAIT connect_base_builder(settings, std::move(service), context);
        }

        auto accept_base(
            const json::v1::object& settings,
            std::shared_ptr<rpc::service> service,
            const layered_connection_context& context) const -> CORO_TASK(stream_acceptor_result) override
        {
            CO_RETURN CO_AWAIT accept_base_builder(settings, std::move(service), context);
        }

        auto accept_single_base(
            const json::v1::object& settings,
            std::shared_ptr<rpc::service> service,
            const layered_connection_context& context) const -> CORO_TASK(stream_result) override
        {
            CO_RETURN CO_AWAIT accept_single_base_builder(settings, std::move(service), context);
        }

        auto wrap_stream(
            std::shared_ptr<::streaming::stream> stream,
            const json::v1::object& settings,
            layer_direction direction,
            const layered_connection_context& context) const -> CORO_TASK(stream_result) override
        {
            CO_RETURN CO_AWAIT stream_layer_builder_fn(std::move(stream), settings, direction, context);
        }
    };

    using stream_component_map = std::unordered_map<std::string, std::shared_ptr<const stream_component_factory>>;

    class transport_component_factory
    {
    public:
        virtual ~transport_component_factory() = default;

        virtual auto connect_transport(
            const json::v1::object&,
            const connection_settings&,
            std::shared_ptr<rpc::service>) const -> transport_connect_context
        {
            return {rpc::error::INVALID_DATA(), {}, {}, {}};
        }
    };

    using transport_component_map = std::unordered_map<std::string, std::shared_ptr<const transport_component_factory>>;

    transport_connect_context make_transport_connect_context(
        const typed_settings& transport_settings,
        const connection_settings& settings,
        std::shared_ptr<rpc::service> service);

    template<class Options> auto materialise_layer_settings(const json::v1::object& options) -> std::optional<Options>
    {
        auto materialised = materialise_settings<Options>(options);
        if (materialised.error_code != rpc::error::OK())
            return std::nullopt;
        return std::move(materialised.settings);
    }

    void register_tcp_blocking_stream_components(stream_component_map& components);
    void register_tcp_coroutine_stream_components(stream_component_map& components);
    void register_spsc_queue_stream_components(stream_component_map& components);

    void register_local_transport_components(transport_component_map& components);
    void register_sgx_blocking_transport_components(transport_component_map& components);
    void register_sgx_coroutine_transport_components(transport_component_map& components);
} // namespace rpc::connection_factory::detail
