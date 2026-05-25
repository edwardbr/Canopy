/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <cstdint>

#include <io_uring/host_io_uring.h>
#include <streaming/io_uring/acceptor.h>
#include <streaming/io_uring/connector.h>
#include <transport/tests/streaming_setup_base.h>

template<bool UseHostInChild, bool RunStandardTests, bool CreateNewZoneThenCreateSubordinatedZone>
class streaming_io_uring_setup
    : public streaming_setup_base<UseHostInChild, RunStandardTests, CreateNewZoneThenCreateSubordinatedZone>
{
    using base = streaming_setup_base<UseHostInChild, RunStandardTests, CreateNewZoneThenCreateSubordinatedZone>;

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

        rpc::io_uring::linux_io_uring_handle::options options;
        options.queue_depth = 256;
        options.use_sqpoll = true;
        options.buffer_count = 256;
        options.buffer_size = 4096;
        options.register_buffers = false;
        options.fixed_file_count = 128;
        options.register_fixed_files = true;

        auto ret = rpc::io_uring::create_scheduler(io_uring_scheduler_owner_, options, this->io_scheduler_);
        if (ret != rpc::error::OK())
        {
            RPC_ERROR("Failed to create io_uring scheduler: {}", ret);
            CO_RETURN false;
        }

        auto controller = io_uring_scheduler_owner_->get_controller();
        if (!controller)
        {
            RPC_ERROR("io_uring scheduler did not create a controller");
            CO_RETURN false;
        }

        acceptor_ = std::make_shared<streaming::io_uring::acceptor>(controller);

        uint16_t port = 0;
        int last_listen_error = rpc::error::OK();
        for (uint16_t candidate_port = first_port_; candidate_port < last_port_; ++candidate_port)
        {
            last_listen_error = CO_AWAIT acceptor_->listen_loopback(candidate_port);
            if (last_listen_error == rpc::error::OK())
            {
                port = candidate_port;
                break;
            }
        }

        if (port == 0)
        {
            RPC_ERROR("Failed to start io_uring listener: {}", last_listen_error);
            CO_RETURN false;
        }

        this->listener_ = std::make_unique<streaming::listener>(
            "responder_transport", acceptor_, this->template make_test_connection_callback<yyy::i_host, yyy::i_example>());

        if (!this->listener_->start_listening(this->peer_service_))
        {
            RPC_ERROR("Failed to start io_uring listener task");
            CO_RETURN false;
        }

        auto stream_result = CO_AWAIT streaming::io_uring::connect_loopback(controller, port);
        if (stream_result.error_code != rpc::error::OK() || !stream_result.connection)
        {
            RPC_ERROR(
                "Failed to connect io_uring client to server error_code={} native_result={}",
                stream_result.error_code,
                stream_result.native_result);
            CO_RETURN false;
        }

        this->initiator_transport_ = rpc::stream_transport::make_client(
            "initiator_transport", this->root_service_, std::move(stream_result.connection), this->test_transport_options_);

        auto connect_to_zone_result = CO_AWAIT this->root_service_->template connect_to_zone<yyy::i_host, yyy::i_example>(
            "main child", this->initiator_transport_, hst);
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
        if (this->listener_)
        {
            CO_AWAIT this->listener_->stop_listening();
            this->listener_.reset();
        }
        acceptor_.reset();
        CO_RETURN;
    }

public:
    ~streaming_io_uring_setup() override = default;

    void tear_down() override
    {
        base::tear_down();
        if (io_uring_scheduler_owner_)
        {
            io_uring_scheduler_owner_->shutdown();
            io_uring_scheduler_owner_.reset();
        }
    }

private:
    static constexpr uint16_t first_port_{26000};
    static constexpr uint16_t last_port_{26064};

    std::shared_ptr<rpc::io_uring::io_uring_scheduler> io_uring_scheduler_owner_;
    std::shared_ptr<streaming::io_uring::acceptor> acceptor_;
};
