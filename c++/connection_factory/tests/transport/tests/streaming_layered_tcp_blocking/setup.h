/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <cstdint>
#include <string>

#include <connection_factory/connection_factory.h>
#include <tcp_blocking_stream/tcp_blocking_stream_config.h>
#include <transport/tests/streaming_setup_base.h>

template<bool UseHostInChild, bool RunStandardTests, bool CreateNewZoneThenCreateSubordinatedZone>
class streaming_layered_tcp_blocking_setup
    : public streaming_setup_base<UseHostInChild, RunStandardTests, CreateNewZoneThenCreateSubordinatedZone>
{
    std::shared_ptr<rpc::connection_factory::listener_handle> rpc_listener_;

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

        ::rpc::tcp_blocking_stream::endpoint endpoint;
        endpoint.host = std::string("127.0.0.1");
        endpoint.port = uint16_t{8081};

        rpc::stream_layers::stream_layer_settings layer;
        layer.type = "tcp_blocking";
        layer.settings = to_json_object(endpoint);
        options.stream_layers.push_back(std::move(layer));
        return options;
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

        auto options = make_options();
        auto accept_result = CO_AWAIT rpc::connection_factory::accept_rpc<yyy::i_host, yyy::i_example>(
            this->make_interface_setup_factory(),
            options,
            this->peer_service_,
            {},
            [this](std::shared_ptr<rpc::stream_transport::transport> transport)
            { this->responder_transport_ = std::move(transport); });

        if (accept_result.error_code != rpc::error::OK() || !accept_result.listener)
        {
            RPC_ERROR("Failed to start layered TCP listener");
            CO_RETURN false;
        }
        rpc_listener_ = std::move(accept_result.listener);

        auto connect_result = CO_AWAIT rpc::connection_factory::connect_rpc<yyy::i_host, yyy::i_example>(
            hst, options, this->root_service_);
        this->i_example_ptr_ = std::move(connect_result.output_interface);

        if (connect_result.error_code != rpc::error::OK())
        {
            RPC_ERROR("Failed to connect layered TCP zone: {}", connect_result.error_code);
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
    ~streaming_layered_tcp_blocking_setup() override = default;
};
