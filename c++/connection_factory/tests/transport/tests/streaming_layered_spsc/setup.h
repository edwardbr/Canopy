/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include <connection_factory/connection_factory.h>
#include <connection_factory/context.h>
#include <streaming/spsc_queue/factory.h>
#include <transport/tests/streaming_setup_base.h>

template<bool UseHostInChild, bool RunStandardTests, bool CreateNewZoneThenCreateSubordinatedZone>
class streaming_layered_spsc_setup
    : public streaming_setup_base<UseHostInChild, RunStandardTests, CreateNewZoneThenCreateSubordinatedZone>
{
    rpc::spsc_queue::queue_pair queues_;
    std::atomic<uint32_t> connect_layer_calls_{0};
    std::atomic<uint32_t> accept_layer_calls_{0};

    static rpc::connection_factory::connection_settings make_options()
    {
        rpc::connection_factory::connection_settings options;
        using json::v1::convert::to_json_object;

        rpc::stream_transport::transport_settings transport;
        transport.call_timeout_sweep = uint64_t{1};
        rpc::connection_factory::typed_settings transport_settings;
        transport_settings.type = "stream_rpc";
        transport_settings.settings = to_json_object(transport);
        options.transport = std::move(transport_settings);

        rpc::stream_layers::stream_layer_settings layer;
        layer.type = "spsc_queue";
        options.stream_layers.push_back(std::move(layer));

#ifdef CANOPY_CONNECTION_FACTORY_HAS_WEBSOCKET
        rpc::stream_layers::stream_layer_settings websocket_layer;
        websocket_layer.type = "websocket";
        websocket_layer.settings = json::v1::parse(R"json({"keep_alive": {"enabled": false}})json");
        options.stream_layers.push_back(std::move(websocket_layer));
#endif

        rpc::stream_layers::stream_layer_settings alpha_layer;
        alpha_layer.type = "test_passthrough_alpha";
        alpha_layer.settings = json::v1::parse(R"json({"name": "alpha"})json");
        options.stream_layers.push_back(std::move(alpha_layer));

        rpc::stream_layers::stream_layer_settings beta_layer;
        beta_layer.type = "test_passthrough_beta";
        beta_layer.settings = json::v1::parse(R"json({"name": "beta"})json");
        options.stream_layers.push_back(std::move(beta_layer));
        return options;
    }

    rpc::connection_factory::context make_layer_context()
    {
        rpc::connection_factory::context context;
        context.set_spsc_queues(queues_);
        register_passthrough_layer(context, "test_passthrough_alpha");
        register_passthrough_layer(context, "test_passthrough_beta");
        return context;
    }

    void register_passthrough_layer(
        rpc::connection_factory::context& factory_context,
        const std::string& type)
    {
        factory_context.register_stream_layer<rpc::connection_factory::service_settings>(
            type,
            [this, type](
                std::shared_ptr<::streaming::stream> stream,
                rpc::connection_factory::service_settings settings,
                rpc::connection_factory::layer_direction direction,
                const rpc::connection_factory::context&) -> CORO_TASK(rpc::connection_factory::stream_result)
            {
                if (!settings.name)
                    CO_RETURN rpc::connection_factory::stream_result{rpc::error::INVALID_DATA(), {}};

                const auto expected_marker = type == "test_passthrough_alpha" ? "alpha" : "beta";
                if (settings.name.value() != expected_marker)
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
        this->root_service_ = rpc::root_service::create("host", root_zone_id, this->io_scheduler_);
        this->peer_service_ = rpc::root_service::create("peer", peer_zone_id, this->io_scheduler_);
        current_host_service = this->root_service_;
        queues_ = rpc::spsc_queue::queue_pair::create();

        connect_layer_calls_.store(0, std::memory_order_relaxed);
        accept_layer_calls_.store(0, std::memory_order_relaxed);

        auto options = make_options();
        auto context = make_layer_context();
        auto accept_result = CO_AWAIT rpc::connection_factory::accept_rpc<yyy::i_host, yyy::i_example>(
            this->make_interface_setup_factory(), options, this->peer_service_, context);
        if (accept_result.error_code != rpc::error::OK() || !accept_result.connection)
            CO_RETURN false;
        this->responder_transport_ = accept_result.connection->transport();

        rpc::shared_ptr<yyy::i_host> hst(new host());
        this->local_host_ptr_ = hst;

        auto connect_result = CO_AWAIT rpc::connection_factory::connect_rpc<yyy::i_host, yyy::i_example>(
            hst, options, this->root_service_, std::move(context));
        this->i_example_ptr_ = std::move(connect_result.output_interface);

        if (connect_result.error_code != rpc::error::OK())
            CO_RETURN false;

        if (connect_layer_calls_.load(std::memory_order_relaxed) != 2
            || accept_layer_calls_.load(std::memory_order_relaxed) != 2)
        {
            RPC_ERROR(
                "Layered SPSC setup did not apply all configured stream layers: connect={} accept={}",
                connect_layer_calls_.load(std::memory_order_relaxed),
                accept_layer_calls_.load(std::memory_order_relaxed));
            CO_RETURN false;
        }

        CO_RETURN true;
    }

public:
    ~streaming_layered_spsc_setup() override = default;
};
