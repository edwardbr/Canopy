/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <streaming/spsc_queue/stream.h>
#include <transport/tests/streaming_setup_base.h>

template<bool UseHostInChild, bool RunStandardTests, bool CreateNewZoneThenCreateSubordinatedZone>
class streaming_spsc_setup
    : public streaming_setup_base<UseHostInChild, RunStandardTests, CreateNewZoneThenCreateSubordinatedZone>
{
    streaming::spsc_queue::queue_type send_spsc_queue_;
    streaming::spsc_queue::queue_type receive_spsc_queue_;

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
            = std::make_shared<streaming::spsc_queue::stream>(&receive_spsc_queue_, &send_spsc_queue_, io_sched);
        this->responder_transport_ = std::static_pointer_cast<rpc::stream_transport::transport>(
            CO_AWAIT this->peer_service_->template make_acceptor<yyy::i_host, yyy::i_example>("responder_transport",
                rpc::stream_transport::transport_factory(std::move(peer_stream)),
                this->make_interface_setup_factory()));

        CO_AWAIT this->responder_transport_->accept();

        rpc::shared_ptr<yyy::i_host> hst(new host());
        this->local_host_ptr_ = hst;

        auto client_stream
            = std::make_shared<streaming::spsc_queue::stream>(&send_spsc_queue_, &receive_spsc_queue_, io_sched);
        this->initiator_transport_
            = rpc::stream_transport::make_client("initiator_transport", this->root_service_, std::move(client_stream));

        auto connect_result = CO_AWAIT this->root_service_->template connect_to_zone<yyy::i_host, yyy::i_example>(
            "main child", this->initiator_transport_, hst);
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
