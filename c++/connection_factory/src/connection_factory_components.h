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
#include <vector>

#include <connection_factory/context.h>
#include <connection_factory/detail/service.h>

namespace rpc::connection_factory::detail
{
#ifndef CANOPY_SCHEMA_ID_BASE
#  define CANOPY_SCHEMA_ID_BASE "https://schemas.canopy.dev/"
#endif

    enum class component_role
    {
        base_stream,
        stream_layer,
        transport,
        runtime_dependency,
    };

    enum class component_status
    {
        available,
        experimental,
        planned,
    };

    struct component_descriptor
    {
        std::string type;
        component_role role{component_role::runtime_dependency};
        component_status status{component_status::available};
        std::string settings_schema_id;
        std::string settings_definition;
    };

    inline auto schema_id(const char* path) -> std::string
    {
        return std::string(CANOPY_SCHEMA_ID_BASE) + path;
    }

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
            json::v1::object,
            std::shared_ptr<rpc::service>,
            context) const -> CORO_TASK(stream_result)
        {
            CO_RETURN stream_result{rpc::error::INVALID_DATA(), {}};
        }

        virtual auto accept_base(
            json::v1::object,
            std::shared_ptr<rpc::service>,
            context) const -> CORO_TASK(stream_acceptor_result)
        {
            CO_RETURN stream_acceptor_result{rpc::error::INVALID_DATA(), {}, {}, 0};
        }

        virtual auto accept_single_base(
            json::v1::object,
            std::shared_ptr<rpc::service>,
            context) const -> CORO_TASK(stream_result)
        {
            CO_RETURN stream_result{rpc::error::INVALID_DATA(), {}};
        }

        virtual auto wrap_stream(
            std::shared_ptr<::streaming::stream>,
            json::v1::object,
            layer_direction,
            context) const -> CORO_TASK(stream_result)
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
            json::v1::object settings,
            std::shared_ptr<rpc::service> service,
            context factory_context) const -> CORO_TASK(stream_result) override
        {
            CO_RETURN CO_AWAIT connect_base_builder(std::move(settings), std::move(service), std::move(factory_context));
        }

        auto accept_base(
            json::v1::object settings,
            std::shared_ptr<rpc::service> service,
            context factory_context) const -> CORO_TASK(stream_acceptor_result) override
        {
            CO_RETURN CO_AWAIT accept_base_builder(std::move(settings), std::move(service), std::move(factory_context));
        }

        auto accept_single_base(
            json::v1::object settings,
            std::shared_ptr<rpc::service> service,
            context factory_context) const -> CORO_TASK(stream_result) override
        {
            CO_RETURN CO_AWAIT accept_single_base_builder(
                std::move(settings), std::move(service), std::move(factory_context));
        }

        auto wrap_stream(
            std::shared_ptr<::streaming::stream> stream,
            json::v1::object settings,
            layer_direction direction,
            context factory_context) const -> CORO_TASK(stream_result) override
        {
            CO_RETURN CO_AWAIT stream_layer_builder_fn(
                std::move(stream), std::move(settings), direction, std::move(factory_context));
        }
    };

    struct stream_component_entry
    {
        component_descriptor descriptor;
        std::shared_ptr<const stream_component_factory> factory;
    };

    using stream_component_map = std::unordered_map<std::string, stream_component_entry>;

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

    struct transport_component_entry
    {
        component_descriptor descriptor;
        std::shared_ptr<const transport_component_factory> factory;
    };

    using transport_component_map = std::unordered_map<std::string, transport_component_entry>;

    const std::vector<component_descriptor>& built_in_stream_component_descriptors();
    const std::vector<component_descriptor>& built_in_stream_layer_descriptors();
    const std::vector<component_descriptor>& built_in_transport_component_descriptors();

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
    void register_blocking_dll_components(transport_component_map& components);
    void register_ipc_spsc_components(transport_component_map& components);
    void register_unshared_scheduler_dll_components(transport_component_map& components);
    void register_shared_scheduler_dll_components(transport_component_map& components);
} // namespace rpc::connection_factory::detail
