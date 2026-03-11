/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <streaming/io_uring_stream_acceptor.h>
#include <transport/tests/streaming_setup_base.h>

template<bool UseHostInChild, bool RunStandardTests, bool CreateNewZoneThenCreateSubordinatedZone>
class streaming_io_uring_setup
    : public streaming_setup_base<UseHostInChild, RunStandardTests, CreateNewZoneThenCreateSubordinatedZone>
{
protected:
    CORO_TASK(bool) do_coro_setup() override
    {
        auto root_zone_id = rpc::DEFAULT_PREFIX;
        auto peer_zone_id = rpc::DEFAULT_PREFIX;
        peer_zone_id.set_subnet(peer_zone_id.get_subnet() + 1);

        this->peer_service_ = std::make_shared<rpc::root_service>("peer", peer_zone_id, this->io_scheduler_);
        this->root_service_ = std::make_shared<rpc::root_service>("host", root_zone_id, this->io_scheduler_);
        current_host_service = this->root_service_;

        rpc::shared_ptr<yyy::i_host> hst(new host());
        this->local_host_ptr_ = hst;

        auto rpc_handler = this->make_connection_handler();

        this->listener_ = std::make_unique<streaming::listener>(
            std::make_shared<streaming::io_uring_stream_acceptor>(coro::net::socket_address{"127.0.0.1", 8082}),
            [this, peer_service = this->peer_service_, rpc_handler](
                std::shared_ptr<streaming::stream> stream) -> CORO_TASK(void)
            {
                this->responder_transport_ = rpc::stream_transport::transport::create(
                    "responder_transport", peer_service, std::move(stream), rpc_handler);
                CO_RETURN;
            });

        if (!this->listener_->start_listening(this->peer_service_))
        {
            RPC_ERROR("Failed to start io_uring listener");
            CO_RETURN false;
        }

        auto scheduler = this->root_service_->get_scheduler();
        coro::net::tcp::client client(scheduler, coro::net::socket_address{"127.0.0.1", 8082});

        auto connection_status = CO_AWAIT client.connect(std::chrono::milliseconds(5000));
        if (connection_status != coro::net::connect_status::connected)
        {
            RPC_ERROR("Failed to connect io_uring client to server (status: {})", static_cast<int>(connection_status));
            CO_RETURN false;
        }

        auto io_uring_stm = std::make_shared<streaming::io_uring_tcp_stream>(std::move(client), scheduler);
        this->initiator_transport_ = rpc::stream_transport::transport::create(
            "initiator_transport", this->root_service_, std::move(io_uring_stm), nullptr);

        auto ret = CO_AWAIT this->root_service_->connect_to_zone(
            "main child", this->initiator_transport_, hst, this->i_example_ptr_);

        if (ret != rpc::error::OK())
        {
            RPC_ERROR("Failed to connect to zone: {}", ret);
            CO_RETURN false;
        }

        CO_RETURN true;
    }

    CORO_TASK(void) do_coro_teardown() override
    {
        if (this->listener_)
        {
            CO_AWAIT this->listener_->stop_listening();
            this->listener_.reset();
        }
        CO_RETURN;
    }

public:
    virtual ~streaming_io_uring_setup() = default;
};
