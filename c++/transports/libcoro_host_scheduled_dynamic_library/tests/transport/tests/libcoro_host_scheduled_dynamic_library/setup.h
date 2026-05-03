/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#ifdef CANOPY_BUILD_COROUTINE

#  include "test_host.h"
#  include "test_globals.h"
#  include <atomic>
#  include <chrono>
#  include <gtest/gtest.h>
#  include <thread>
#  include <vector>

#  include <rpc/rpc.h>
#  include <common/tests.h>
#  include <common/transport_setup_base.h>
#  include <transports/libcoro_host_scheduled_dynamic_library/transport.h>

#  ifndef CANOPY_TEST_LIBCORO_HOST_SCHEDULED_DLL_PATH
#    error "CANOPY_TEST_LIBCORO_HOST_SCHEDULED_DLL_PATH must be defined (set by CMake)"
#  endif

template<bool UseHostInChild, bool RunStandardTests, bool CreateNewZoneThenCreateSubordinatedZone>
class libcoro_host_scheduled_transport_setup
    : public transport_setup_base<UseHostInChild, RunStandardTests, CreateNewZoneThenCreateSubordinatedZone>
{
    std::atomic_bool startup_complete_ = false;
    std::vector<std::weak_ptr<rpc::libcoro_host_scheduled_dynamic_library::child_transport>> child_transports_;

public:
    ~libcoro_host_scheduled_transport_setup() override = default;

    CORO_TASK(bool) CoroSetUp()
    {
        this->start_telemetry_test();

        this->root_service_ = rpc::root_service::create("host", rpc::DEFAULT_PREFIX, this->io_scheduler_);
        current_host_service = this->root_service_;

        rpc::shared_ptr<yyy::i_host> hst(new host());
        this->local_host_ptr_ = hst;

        auto child_transport = std::make_shared<rpc::libcoro_host_scheduled_dynamic_library::child_transport>(
            "main child", this->root_service_, CANOPY_TEST_LIBCORO_HOST_SCHEDULED_DLL_PATH);
        child_transports_.push_back(child_transport);

        auto connect_result = CO_AWAIT this->root_service_->template connect_to_zone<yyy::i_host, yyy::i_example>(
            "main child", child_transport, hst);
        this->i_example_ptr_ = std::move(connect_result.output_interface);
        startup_complete_.store(true);
        if (connect_result.error_code != rpc::error::OK())
        {
            CO_RETURN false;
        }
        CO_RETURN true;
    }

    virtual void set_up()
    {
        this->io_scheduler_ = std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
            coro::scheduler::options{.thread_strategy = coro::scheduler::thread_strategy_t::manual,
                .pool = coro::thread_pool::options{
                    .thread_count = 1,
                }}));

        RPC_ASSERT(this->io_scheduler_->spawn_detached(this->check_for_error(CoroSetUp())));
        while (!startup_complete_.load())
        {
            this->io_scheduler_->process_events(std::chrono::milliseconds(1));
        }
        while (this->io_scheduler_->process_events(std::chrono::milliseconds(1)) > 0)
        {
        }

        ASSERT_EQ(this->error_has_occurred_, false);
    }

    virtual void tear_down()
    {
        auto scheduler = this->io_scheduler_;
        auto root_shutdown_event = this->make_root_shutdown_event_for_test();
        this->release_interfaces_and_root_service_for_test(root_shutdown_event);
        while (scheduler->process_events(std::chrono::milliseconds(1)) > 0)
        {
        }

        // The host-scheduled DLL executes DLL code on the host scheduler's
        // worker threads.  Stop those threads before the scheduler shared_ptr
        // count drops to zero; the transport's self-release waits for scheduler
        // expiry before dlclose, and dlclose is only safe after the workers have
        // joined and run their DLL-owned TLS destructors.
        scheduler->shutdown();

        this->io_scheduler_ = nullptr;
        scheduler.reset();

        for (auto attempt = 0; attempt < 1000; ++attempt)
        {
            auto all_expired = true;
            for (auto& child_transport : child_transports_)
                all_expired = all_expired && child_transport.expired();
            if (all_expired)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        for (auto& child_transport : child_transports_)
            EXPECT_TRUE(child_transport.expired());
        child_transports_.clear();

        this->reset_telemetry_for_test();
    }

    CORO_TASK(rpc::shared_ptr<yyy::i_example>) create_new_zone()
    {
        rpc::shared_ptr<yyy::i_host> hst;
        if (this->use_host_in_child_)
            hst = this->local_host_ptr_.lock();

        auto child_transport = std::make_shared<rpc::libcoro_host_scheduled_dynamic_library::child_transport>(
            "new child", this->root_service_, CANOPY_TEST_LIBCORO_HOST_SCHEDULED_DLL_PATH);
        child_transports_.push_back(child_transport);

        auto connect_result = CO_AWAIT this->root_service_->template connect_to_zone<yyy::i_host, yyy::i_example>(
            "new child", child_transport, hst);
        rpc::shared_ptr<yyy::i_example> example_relay_ptr = std::move(connect_result.output_interface);

        if (CreateNewZoneThenCreateSubordinatedZone)
        {
            rpc::shared_ptr<yyy::i_example> new_ptr;
            auto err = CO_AWAIT example_relay_ptr->create_example_in_subordinate_zone(
                new_ptr, this->use_host_in_child_ ? hst : nullptr);
            if (err == rpc::error::OK())
            {
                CO_AWAIT example_relay_ptr->set_host(nullptr);
                example_relay_ptr = new_ptr;
            }
            else
            {
                RPC_ASSERT(false);
            }
        }
        CO_RETURN example_relay_ptr;
    }
};

#endif // CANOPY_BUILD_COROUTINE
