/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <cstdint>

#include <connection_factory/tcp_coroutine.h>
#include <transport/tests/streaming_setup_base.h>

template<bool UseHostInChild, bool RunStandardTests, bool CreateNewZoneThenCreateSubordinatedZone>
class streaming_tcp_coroutine_setup
    : public streaming_setup_base<UseHostInChild, RunStandardTests, CreateNewZoneThenCreateSubordinatedZone>
{
    using base = streaming_setup_base<UseHostInChild, RunStandardTests, CreateNewZoneThenCreateSubordinatedZone>;

    std::shared_ptr<rpc::connection_factory::listener_handle> rpc_listener_;

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

        ::rpc::tcp_coroutine_stream::endpoint accept_tcp_coroutine_options;

        rpc::connection_factory::stream_rpc_connection_settings accept_factory_options;
        accept_factory_options.transport.call_timeout = uint64_t{30000};
        accept_factory_options.transport.call_timeout_sweep = uint64_t{1};

        auto accept_result = CO_AWAIT rpc::tcp_coroutine::accept_rpc<yyy::i_host, yyy::i_example>(
            this->make_interface_setup_factory(),
            accept_tcp_coroutine_options,
            accept_factory_options,
            this->peer_service_,
            [this](std::shared_ptr<rpc::stream_transport::transport> transport)
            { this->responder_transport_ = std::move(transport); });

        if (accept_result.error_code != rpc::error::OK() || !accept_result.handle)
        {
            RPC_ERROR("Failed to start TCP coroutine listener task");
            CO_RETURN false;
        }
        rpc_listener_ = std::move(accept_result.handle);

        ::rpc::tcp_coroutine_stream::endpoint connect_tcp_coroutine_options;
        connect_tcp_coroutine_options.port = rpc_listener_->port();

        rpc::connection_factory::stream_rpc_connection_settings connect_factory_options;
        connect_factory_options.transport.call_timeout = uint64_t{30000};
        connect_factory_options.transport.call_timeout_sweep = uint64_t{1};

        auto connect_to_zone_result = CO_AWAIT rpc::tcp_coroutine::connect_rpc<yyy::i_host, yyy::i_example>(
            hst, connect_tcp_coroutine_options, connect_factory_options, this->root_service_);
        this->i_example_ptr_ = std::move(connect_to_zone_result.output_interface);

        if (connect_to_zone_result.error_code != rpc::error::OK())
        {
            RPC_ERROR("Failed to connect to zone: {}", connect_to_zone_result.error_code);
            CO_RETURN false;
        }

        CO_RETURN true;
    }

    CORO_TASK(void) do_coro_teardown() override
    {
        if (rpc_listener_)
        {
            CO_AWAIT rpc_listener_->stop();
        }
        CO_RETURN;
    }

public:
    ~streaming_tcp_coroutine_setup() override = default;

    void tear_down() override
    {
        base::tear_down();
        rpc_listener_.reset();
    }

private:
};
