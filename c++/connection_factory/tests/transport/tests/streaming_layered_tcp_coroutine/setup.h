/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include <connection_factory/connection_factory.h>
#include <tcp_coroutine_stream/tcp_coroutine_stream_config.h>
#include <transport/tests/streaming_setup_base.h>

template<bool UseHostInChild, bool RunStandardTests, bool CreateNewZoneThenCreateSubordinatedZone>
class streaming_layered_tcp_coroutine_setup
    : public streaming_setup_base<UseHostInChild, RunStandardTests, CreateNewZoneThenCreateSubordinatedZone>
{
    std::shared_ptr<rpc::connection_factory::listener_handle> rpc_listener_;
    std::atomic<uint32_t> connect_layer_calls_{0};
    std::atomic<uint32_t> accept_layer_calls_{0};

    static rpc::connection_factory_config::connection_settings make_options()
    {
        rpc::connection_factory_config::connection_settings options;
        using json::v1::convert::to_json_object;

        rpc::stream_transport::transport_settings transport;
        transport.call_timeout = uint64_t{30000};
        transport.call_timeout_sweep = uint64_t{1};
        rpc::connection_factory_config::typed_settings transport_settings;
        transport_settings.type = "stream_rpc";
        transport_settings.settings = to_json_object(transport);
        options.transport = std::move(transport_settings);

        ::rpc::tcp_coroutine_stream::endpoint endpoint;
        endpoint.host = std::string("127.0.0.1");
        endpoint.port = uint16_t{8081};

        rpc::stream_layers::stream_layer_settings layer;
        layer.type = "tcp_coroutine";
        layer.settings = to_json_object(endpoint);
        options.stream_layers.push_back(std::move(layer));

#ifdef CANOPY_CONNECTION_FACTORY_HAS_WEBSOCKET
        rpc::stream_layers::stream_layer_settings websocket_layer;
        websocket_layer.type = "websocket";
        websocket_layer.settings = json::v1::parse(R"json({"keep_alive": {"enabled": false}})json");
        options.stream_layers.push_back(std::move(websocket_layer));
#endif

        rpc::stream_layers::stream_layer_settings alpha_layer;
        alpha_layer.type = "test_passthrough_alpha";
        alpha_layer.settings = json::v1::parse(R"json({"marker": "alpha"})json");
        options.stream_layers.push_back(std::move(alpha_layer));

        rpc::stream_layers::stream_layer_settings beta_layer;
        beta_layer.type = "test_passthrough_beta";
        beta_layer.settings = json::v1::parse(R"json({"marker": "beta"})json");
        options.stream_layers.push_back(std::move(beta_layer));
        return options;
    }

    rpc::connection_factory::layered_connection_context make_layer_context()
    {
        rpc::connection_factory::layered_connection_context context;
        register_passthrough_layer(context, "test_passthrough_alpha");
        register_passthrough_layer(context, "test_passthrough_beta");
        return context;
    }

    void register_passthrough_layer(
        rpc::connection_factory::layered_connection_context& context,
        const std::string& type)
    {
        context.register_stream_layer(
            type,
            [this, type](
                std::shared_ptr<::streaming::stream> stream,
                const json::v1::object& settings,
                rpc::connection_factory::layer_direction direction,
                const rpc::connection_factory::layered_connection_context&) -> CORO_TASK(rpc::connection_factory::stream_result)
            {
                const auto& settings_map = settings.as_map();
                const auto marker = settings_map.find("marker");
                if (marker == settings_map.end() || marker->second.get_type() != json::v1::object::type::string_type)
                    CO_RETURN rpc::connection_factory::stream_result{rpc::error::INVALID_DATA(), {}};

                const auto expected_marker = type == "test_passthrough_alpha" ? "alpha" : "beta";
                if (marker->second.get<std::string>() != expected_marker)
                    CO_RETURN rpc::connection_factory::stream_result{rpc::error::INVALID_DATA(), {}};

                if (direction == rpc::connection_factory::layer_direction::connect)
                    connect_layer_calls_.fetch_add(1, std::memory_order_relaxed);
                else
                    accept_layer_calls_.fetch_add(1, std::memory_order_relaxed);

                CO_RETURN rpc::connection_factory::stream_result{rpc::error::OK(), std::move(stream)};
            });
    }

protected:
    CORO_TASK(bool) do_coro_setup() override
    {
        auto root_zone_id = rpc::DEFAULT_PREFIX;
        auto peer_zone_id = this->make_peer_zone_id();

        this->peer_service_ = rpc::root_service::create("peer", peer_zone_id, this->io_scheduler_);
        this->root_service_ = rpc::root_service::create("host", root_zone_id, this->io_scheduler_);
        current_host_service = this->root_service_;

        rpc::shared_ptr<yyy::i_host> hst(new host());
        this->local_host_ptr_ = hst;

        connect_layer_calls_.store(0, std::memory_order_relaxed);
        accept_layer_calls_.store(0, std::memory_order_relaxed);

        auto options = make_options();
        auto layer_context = make_layer_context();
        auto accept_result = CO_AWAIT rpc::connection_factory::accept_rpc<yyy::i_host, yyy::i_example>(
            this->make_interface_setup_factory(),
            options,
            this->peer_service_,
            layer_context,
            [this](std::shared_ptr<rpc::stream_transport::transport> transport)
            { this->responder_transport_ = std::move(transport); });

        if (accept_result.error_code != rpc::error::OK() || !accept_result.listener)
        {
            RPC_ERROR("Failed to start layered TCP coroutine listener");
            CO_RETURN false;
        }
        rpc_listener_ = std::move(accept_result.listener);

        auto connect_result = CO_AWAIT rpc::connection_factory::connect_rpc<yyy::i_host, yyy::i_example>(
            hst, options, this->root_service_, std::move(layer_context));
        this->i_example_ptr_ = std::move(connect_result.output_interface);

        if (connect_result.error_code != rpc::error::OK())
        {
            RPC_ERROR("Failed to connect layered TCP coroutine zone: {}", connect_result.error_code);
            CO_RETURN false;
        }

        if (connect_layer_calls_.load(std::memory_order_relaxed) != 2
            || accept_layer_calls_.load(std::memory_order_relaxed) != 2)
        {
            RPC_ERROR(
                "Layered TCP coroutine setup did not apply all configured stream layers: connect={} accept={}",
                connect_layer_calls_.load(std::memory_order_relaxed),
                accept_layer_calls_.load(std::memory_order_relaxed));
            CO_RETURN false;
        }

        CO_RETURN true;
    }

    CORO_TASK(void) do_coro_teardown() override
    {
        if (rpc_listener_)
        {
            CO_AWAIT rpc_listener_->stop();
            rpc_listener_.reset();
        }
        CO_RETURN;
    }

public:
    ~streaming_layered_tcp_coroutine_setup() override = default;
};
