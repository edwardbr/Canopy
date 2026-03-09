/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <streaming/spsc_queue_stream.h>
#include <transport/tests/streaming_setup_base.h>

template<bool UseHostInChild, bool RunStandardTests, bool CreateNewZoneThenCreateSubordinatedZone>
class streaming_spsc_setup
    : public streaming_setup_base<UseHostInChild, RunStandardTests, CreateNewZoneThenCreateSubordinatedZone>
{
    streaming::spsc_raw_queue send_spsc_queue_;
    streaming::spsc_raw_queue receive_spsc_queue_;

protected:
    CORO_TASK(bool) do_coro_setup() override
    {
        auto root_zone_id = rpc::DEFAULT_PREFIX;
        auto peer_zone_id = rpc::DEFAULT_PREFIX;
        peer_zone_id.set_subnet(peer_zone_id.get_subnet() + 1);
        this->root_service_ = std::make_shared<rpc::root_service>("host", root_zone_id, this->io_scheduler_);
        this->peer_service_ = std::make_shared<rpc::root_service>("peer", peer_zone_id, this->io_scheduler_);

        auto io_sched = this->io_scheduler_;
        auto peer_stream
            = std::make_shared<streaming::spsc_queue_stream>(&receive_spsc_queue_, &send_spsc_queue_, io_sched);
        this->responder_transport_ = rpc::stream_transport::streaming_transport::create(
            "responder_transport", this->peer_service_, std::move(peer_stream), this->make_connection_handler());

        CO_AWAIT this->responder_transport_->accept();

        rpc::shared_ptr<yyy::i_host> hst(new host());
        this->local_host_ptr_ = hst;

        auto client_stream
            = std::make_shared<streaming::spsc_queue_stream>(&send_spsc_queue_, &receive_spsc_queue_, io_sched);
        this->initiator_transport_ = rpc::stream_transport::streaming_transport::create(
            "initiator_transport", this->root_service_, std::move(client_stream), nullptr);

        auto ret = CO_AWAIT this->root_service_->connect_to_zone(
            "main child", this->initiator_transport_, hst, this->i_example_ptr_);

        if (ret != rpc::error::OK())
        {
            CO_RETURN false;
        }
        CO_RETURN true;
    }

public:
    virtual ~streaming_spsc_setup() = default;
};
