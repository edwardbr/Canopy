/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <common/transport_setup_base.h>
#include <streaming/listener.h>
#include <transports/streaming/transport.h>

template<bool UseHostInChild, bool RunStandardTests, bool CreateNewZoneThenCreateSubordinatedZone>
class streaming_setup_base
    : public transport_setup_base<UseHostInChild, RunStandardTests, CreateNewZoneThenCreateSubordinatedZone>
{
    using base = transport_setup_base<UseHostInChild, RunStandardTests, CreateNewZoneThenCreateSubordinatedZone>;

    bool setup_complete_ = false;

protected:
    std::shared_ptr<rpc::root_service> peer_service_;

    // The two sides of the transport: initiator (client) and responder (server/child)
    std::shared_ptr<rpc::stream_transport::transport> initiator_transport_;
    std::shared_ptr<rpc::stream_transport::transport> responder_transport_;
    std::unique_ptr<streaming::listener> listener_;

    rpc::stream_transport::transport::connection_handler make_connection_handler()
    {
        return [use_host_in_child = this->use_host_in_child_](const rpc::connection_settings& input_descr,
                   rpc::interface_descriptor& output_interface,
                   std::shared_ptr<rpc::service> service,
                   std::shared_ptr<rpc::stream_transport::transport> transport) -> CORO_TASK(int)
        {
            auto ret = CO_AWAIT service->attach_remote_zone<yyy::i_host, yyy::i_example>("service_proxy",
                transport,
                input_descr,
                output_interface,
                [&](const rpc::shared_ptr<yyy::i_host>& host,
                    rpc::shared_ptr<yyy::i_example>& new_example,
                    const std::shared_ptr<rpc::service>& child_service_ptr) -> CORO_TASK(int)
                {
                    new_example = rpc::shared_ptr<yyy::i_example>(new marshalled_tests::example(child_service_ptr, host));
                    if (use_host_in_child)
                        CO_AWAIT new_example->set_host(host);
                    CO_RETURN rpc::error::OK();
                });
            CO_RETURN ret;
        };
    }

    virtual CORO_TASK(bool) do_coro_setup() = 0;

    virtual CORO_TASK(void) do_coro_teardown() { CO_RETURN; }

public:
    virtual ~streaming_setup_base() = default;

    std::shared_ptr<rpc::stream_transport::transport> get_responder_transport() const { return responder_transport_; }

    CORO_TASK(bool) CoroSetUp()
    {
        this->start_telemetry_test();
        CO_RETURN CO_AWAIT do_coro_setup();
    }

    CORO_TASK(void) CoroTearDown()
    {
        this->i_example_ptr_ = nullptr;
        this->i_host_ptr_ = nullptr;
        this->local_host_ptr_.reset();
        CO_AWAIT do_coro_teardown();
        CO_RETURN;
    }

    virtual void set_up()
    {
        setup_complete_ = false;
        this->io_scheduler_ = std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
            coro::scheduler::options{.thread_strategy = coro::scheduler::thread_strategy_t::manual,
                .pool = coro::thread_pool::options{
                    .thread_count = 1,
                }}));

        auto setup_task = [this]() -> coro::task<void>
        {
            CO_AWAIT this->check_for_error(CoroSetUp());
            setup_complete_ = true;
            CO_RETURN;
        };

        RPC_ASSERT(this->io_scheduler_->spawn_detached(setup_task()));

        while (!setup_complete_)
        {
            this->io_scheduler_->process_events(std::chrono::milliseconds(1));
        }

        ASSERT_EQ(this->error_has_occurred_, false);
    }

    virtual void tear_down()
    {
        bool shutdown_complete = false;
        auto shutdown_task = [&]() -> coro::task<void>
        {
            CO_AWAIT CoroTearDown();
            CO_AWAIT this->io_scheduler_->schedule();
            CO_AWAIT this->io_scheduler_->schedule();
            shutdown_complete = true;
            CO_RETURN;
        };

        RPC_ASSERT(this->io_scheduler_->spawn_detached(shutdown_task()));

        while (!shutdown_complete)
        {
            this->io_scheduler_->process_events(std::chrono::milliseconds(1));
        }

        const int max_iterations = 1000;
        int iteration = 0;
        int disconnected_iterations = 0;
        while (iteration < max_iterations)
        {
            bool all_disconnected = true;

            if (initiator_transport_ && initiator_transport_->get_status() != rpc::transport_status::DISCONNECTED)
                all_disconnected = false;
            if (responder_transport_ && responder_transport_->get_status() != rpc::transport_status::DISCONNECTED)
                all_disconnected = false;

            if (all_disconnected)
            {
                ++disconnected_iterations;
                if (disconnected_iterations > 50)
                    break;
            }

            this->io_scheduler_->process_events(std::chrono::milliseconds(1));
            ++iteration;
        }

        peer_service_.reset();
        this->root_service_.reset();
        this->reset_telemetry_for_test();
    }
};
