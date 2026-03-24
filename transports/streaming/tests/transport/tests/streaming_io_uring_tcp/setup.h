/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <streaming/io_uring/acceptor.h>
#include <streaming/tcp/stream.h>
#include <transport/tests/streaming_setup_base.h>

template<bool UseHostInChild, bool RunStandardTests, bool CreateNewZoneThenCreateSubordinatedZone>
class streaming_io_uring_tcp_setup
    : public streaming_setup_base<UseHostInChild, RunStandardTests, CreateNewZoneThenCreateSubordinatedZone>
{
protected:
    CORO_TASK(bool) do_coro_setup() override
    {
        auto root_zone_id = rpc::DEFAULT_PREFIX;
        auto peer_zone_id = rpc::DEFAULT_PREFIX;
        std::ignore = peer_zone_id.set_subnet(peer_zone_id.get_subnet() + 1);

        this->peer_service_ = std::make_shared<rpc::root_service>("peer", peer_zone_id, this->io_scheduler_);
        this->root_service_ = std::make_shared<rpc::root_service>("host", root_zone_id, this->io_scheduler_);
        current_host_service = this->root_service_;

        rpc::shared_ptr<yyy::i_host> hst(new host());
        this->local_host_ptr_ = hst;

        canopy::network_config::ip_address addr{};
        addr[0] = 127;
        addr[1] = 0;
        addr[2] = 0;
        addr[3] = 1;

        this->listener_ = std::make_unique<streaming::listener>("responder_transport",
            std::make_shared<streaming::io_uring::acceptor>(addr, 8082),
            rpc::stream_transport::make_connection_callback<yyy::i_host, yyy::i_example>(
                this->make_interface_setup_factory()));

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

        auto tcp_stm = std::make_shared<streaming::tcp::stream>(std::move(client), scheduler);
        this->initiator_transport_
            = rpc::stream_transport::make_client("initiator_transport", this->root_service_, std::move(tcp_stm));

        auto connect_result = CO_AWAIT this->root_service_->template connect_to_zone<yyy::i_host, yyy::i_example>(
            "main child", this->initiator_transport_, hst);
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
        if (this->listener_)
        {
            CO_AWAIT this->listener_->stop_listening();
            this->listener_.reset();
        }
        CO_RETURN;
    }

public:
    virtual ~streaming_io_uring_tcp_setup() = default;
};
