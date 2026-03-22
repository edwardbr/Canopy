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
#  include <transports/libcoro_spsc_dynamic_dll/loaded_library.h>
#  include <transports/libcoro_spsc_dynamic_dll/transport.h>

template<bool UseHostInChild, bool RunStandardTests, bool CreateNewZoneThenCreateSubordinatedZone>
class libcoro_spsc_dll_transport_setup
    : public transport_setup_base<UseHostInChild, RunStandardTests, CreateNewZoneThenCreateSubordinatedZone>
{
    rpc::libcoro_spsc_dynamic_dll::queue_pair queues_{};
    std::shared_ptr<rpc::libcoro_spsc_dynamic_dll::loaded_library> loaded_;
    rpc::zone host_zone_ = rpc::DEFAULT_PREFIX;
    rpc::zone dll_zone_ = []
    {
        auto address = rpc::DEFAULT_PREFIX;
        [[maybe_unused]] bool ok = address.set_subnet(address.get_subnet() + 1);
        RPC_ASSERT(ok);
        return rpc::zone(address);
    }();
    std::shared_ptr<rpc::stream_transport::transport> client_transport_;

    CORO_TASK(bool) connect_child()
    {
        client_transport_ = rpc::libcoro_spsc_dynamic_dll::make_client("spsc child", this->root_service_, &queues_);
        auto connect_result = CO_AWAIT this->root_service_->template connect_to_zone<yyy::i_host, yyy::i_example>(
            "spsc child", client_transport_, this->local_host_ptr_.lock());
        this->i_example_ptr_ = std::move(connect_result.output_interface);
        CO_RETURN connect_result.error_code == rpc::error::OK();
    }

public:
    void set_up()
    {
        this->start_telemetry_test();
        this->io_scheduler_ = std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
            coro::scheduler::options{.thread_strategy = coro::scheduler::thread_strategy_t::manual,
                .pool = coro::thread_pool::options{.thread_count = 1}}));

        this->root_service_ = std::make_shared<rpc::root_service>("host", host_zone_, this->io_scheduler_);
        current_host_service = this->root_service_;
        rpc::shared_ptr<yyy::i_host> hst(new host());
        this->i_host_ptr_ = hst;
        this->local_host_ptr_ = hst;

        loaded_ = rpc::libcoro_spsc_dynamic_dll::loaded_library::load(CANOPY_TEST_LIBCORO_SPSC_DLL_PATH,
            "libcoro_spsc_dynamic_dll",
            dll_zone_,
            host_zone_,
            &queues_.dll_to_host,
            &queues_.host_to_dll);
        ASSERT_NE(loaded_, nullptr);

        RPC_ASSERT(this->io_scheduler_->spawn_detached(this->check_for_error(connect_child())));
        while (!this->i_example_ptr_ && !this->error_has_occurred_)
            this->io_scheduler_->process_events(std::chrono::milliseconds(1));
        ASSERT_EQ(this->error_has_occurred_, false);
    }

    void tear_down()
    {
        this->i_host_ptr_ = nullptr;
        this->i_example_ptr_ = nullptr;
        client_transport_.reset();

        for (int idle_iterations = 0; idle_iterations < 10;)
        {
            if (this->io_scheduler_->process_events(std::chrono::milliseconds(1)) == 0)
                ++idle_iterations;
            else
                idle_iterations = 0;
        }

        if (loaded_)
        {
            loaded_->stop();
            loaded_.reset();
        }

        this->root_service_ = nullptr;
        current_host_service.reset();
        this->reset_telemetry_for_test();
    }
};

#endif // CANOPY_BUILD_COROUTINE
