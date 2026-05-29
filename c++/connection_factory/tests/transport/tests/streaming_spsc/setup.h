/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <cstdint>

#include <connection_factory/spsc_queue.h>
#include <transport/tests/streaming_setup_base.h>

template<bool UseHostInChild, bool RunStandardTests, bool CreateNewZoneThenCreateSubordinatedZone>
class streaming_spsc_setup
    : public streaming_setup_base<UseHostInChild, RunStandardTests, CreateNewZoneThenCreateSubordinatedZone>
{
    rpc::spsc_queue::queue_pair queues_;

protected:
    CORO_TASK(bool) do_coro_setup() override
    {
        auto root_zone_id = rpc::DEFAULT_PREFIX;
        auto peer_zone_id = this->make_peer_zone_id();
        this->root_service_ = rpc::root_service::create("host", root_zone_id, this->io_scheduler_);
        this->peer_service_ = rpc::root_service::create("peer", peer_zone_id, this->io_scheduler_);
        current_host_service = this->root_service_;
        queues_ = rpc::spsc_queue::queue_pair::create();

        rpc::connection_factory_config::stream_factory_options common_options;
        common_options.rpc.emplace();
        common_options.rpc->call_timeout_sweep = uint64_t{1};

        auto accept_result = CO_AWAIT rpc::spsc_queue::accept_rpc<yyy::i_host, yyy::i_example>(
            this->make_interface_setup_factory(), queues_, common_options, this->peer_service_);
        if (accept_result.error_code != rpc::error::OK() || !accept_result.handle)
            CO_RETURN false;
        this->responder_transport_ = accept_result.handle->transport();

        rpc::shared_ptr<yyy::i_host> hst(new host());
        this->local_host_ptr_ = hst;

        auto connect_result = CO_AWAIT rpc::spsc_queue::connect_rpc<yyy::i_host, yyy::i_example>(
            hst, queues_, common_options, this->root_service_);
        this->i_example_ptr_ = std::move(connect_result.output_interface);
        auto ret = connect_result.error_code;

        if (ret != rpc::error::OK())
        {
            CO_RETURN false;
        }
        CO_RETURN true;
    }

public:
    ~streaming_spsc_setup() override = default;
};
