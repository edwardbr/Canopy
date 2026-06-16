/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <cstdint>
#include <string>

#include <streaming/tcp_blocking/factory.h>
#include <transport/tests/streaming_setup_base.h>

template<bool UseHostInChild, bool RunStandardTests, bool CreateNewZoneThenCreateSubordinatedZone>
class streaming_tcp_blocking_setup
    : public streaming_setup_base<UseHostInChild, RunStandardTests, CreateNewZoneThenCreateSubordinatedZone>
{
    std::shared_ptr<rpc::stream_transport::listener_handle> rpc_listener_;

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

        ::rpc::tcp_blocking_stream::endpoint endpoint;
        endpoint.host = std::string("127.0.0.1");
        endpoint.port = uint16_t{8080};

        rpc::stream_transport::connection_settings options;
        options.transport.call_timeout = uint64_t{30000};
        options.transport.call_timeout_sweep = uint64_t{1};

        auto accept_result = CO_AWAIT rpc::tcp_blocking::accept_rpc<yyy::i_host, yyy::i_example>(
            this->make_interface_setup_factory(),
            endpoint,
            options,
            this->peer_service_,
            [this](std::shared_ptr<rpc::stream_transport::transport> transport)
            { this->responder_transport_ = std::move(transport); });

        if (accept_result.error_code != rpc::error::OK() || !accept_result.handle)
        {
            RPC_ERROR("Failed to start TCP listener");
            CO_RETURN false;
        }
        rpc_listener_ = std::move(accept_result.handle);

        auto connect_result = CO_AWAIT rpc::tcp_blocking::connect_rpc<yyy::i_host, yyy::i_example>(
            hst, endpoint, options, this->root_service_);
        this->i_example_ptr_ = std::move(connect_result.output_interface);
        auto ret = connect_result.error_code;

        if (ret != rpc::error::OK())
        {
            RPC_ERROR("Failed to connect to zone: {}", ret);
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
    ~streaming_tcp_blocking_setup() override = default;
};
