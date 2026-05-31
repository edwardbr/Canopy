/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#ifdef CANOPY_BUILD_COROUTINE

#  include "test_globals.h"
#  include "test_host.h"

#  include <gtest/gtest.h>

#  include <common/tests.h>
#  include <common/transport_setup_base.h>
#  include <transports/ipc_spsc_transport/loaded_library.h>
#  include <transports/ipc_spsc_transport/queue_transport.h>
#  include <transports/ipc_spsc_transport/transport.h>

#  include <atomic>

template<bool UseHostInChild, bool RunStandardTests, bool CreateNewZoneThenCreateSubordinatedZone>
class ipc_spsc_dll_transport_setup
    : public transport_setup_base<UseHostInChild, RunStandardTests, CreateNewZoneThenCreateSubordinatedZone>
{
    rpc::ipc_spsc_transport::queue_pair queues_{};
    std::shared_ptr<rpc::ipc_spsc_transport::loaded_library> loaded_;
    std::atomic_bool setup_complete_ = false;
    rpc::zone host_zone_ = rpc::DEFAULT_PREFIX;
    rpc::zone dll_zone_ = []
    {
        auto address = rpc::DEFAULT_PREFIX;
        [[maybe_unused]] auto ok = address.set_subnet(address.get_subnet() + 1);
        RPC_ASSERT(ok);
        return rpc::zone(address);
    }();

    CORO_TASK(bool) connect_child()
    {
        auto client_transport = rpc::ipc_spsc_transport::make_client("spsc child", this->root_service_, &queues_);
        auto connect_result = CO_AWAIT this->root_service_->template connect_to_zone<yyy::i_host, yyy::i_example>(
            "spsc child", client_transport, this->local_host_ptr_.lock());
        this->i_example_ptr_ = std::move(connect_result.output_interface);
        setup_complete_.store(true);
        CO_RETURN connect_result.error_code == rpc::error::OK();
    }

public:
    void set_up()
    {
        this->start_telemetry_test();
        this->io_scheduler_ = std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
            coro::scheduler::options{.thread_strategy = coro::scheduler::thread_strategy_t::manual,
                .pool = coro::thread_pool::options{.thread_count = 1}}));

        this->root_service_ = rpc::root_service::create("host", host_zone_, this->io_scheduler_);
        current_host_service = this->root_service_;
        rpc::shared_ptr<yyy::i_host> hst(new host());
        this->i_host_ptr_ = hst;
        this->local_host_ptr_ = hst;

        loaded_ = rpc::ipc_spsc_transport::loaded_library::load(
            CANOPY_TEST_IPC_SPSC_DLL_PATH, "ipc_spsc_transport", dll_zone_, host_zone_, &queues_.dll_to_host, &queues_.host_to_dll);
        ASSERT_NE(loaded_, nullptr);

        setup_complete_.store(false);
        RPC_ASSERT(this->io_scheduler_->spawn_detached(this->check_for_error(connect_child())));
        while (!setup_complete_.load() && !this->error_has_occurred_)
            this->io_scheduler_->process_events(std::chrono::milliseconds(1));
        ASSERT_EQ(this->error_has_occurred_, false);
    }

    void tear_down()
    {
        auto root_shutdown_event = this->make_root_shutdown_event_for_test();
        this->release_interfaces_and_root_service_for_test(root_shutdown_event);

        for (int idle_iterations = 0; idle_iterations < 10;)
        {
            if (this->io_scheduler_->process_events(std::chrono::milliseconds(1)) == 0)
                ++idle_iterations;
            else
                idle_iterations = 0;
        }

        if (loaded_)
        {
            ASSERT_TRUE(loaded_->wait_until_expired(std::chrono::milliseconds{5000}));
            loaded_.reset();
        }

        this->reset_telemetry_for_test();
    }
};

#endif // CANOPY_BUILD_COROUTINE
