/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <atomic>
#include "test_host.h"
#include "test_globals.h"
#include <gtest/gtest.h>

#include <rpc/rpc.h>
#include <common/tests.h>
#include <common/transport_setup_base.h>
#include <transports/local/transport.h>

template<bool UseHostInChild, bool RunStandardTests, bool CreateNewZoneThenCreateSubordinatedZone>
class inproc_setup
    : public transport_setup_base<UseHostInChild, RunStandardTests, CreateNewZoneThenCreateSubordinatedZone>
{
    std::shared_ptr<rpc::child_service> child_service_;
    std::weak_ptr<rpc::child_service> child_service_weak_;

    bool startup_complete_ = false;
    bool shutdown_complete_ = false;

public:
    ~inproc_setup() override = default;

    CORO_TASK(void) check_for_error(CORO_TASK(bool) task)
    {
        auto ret = CO_AWAIT task;
        if (!ret)
        {
            this->error_has_occurred_ = true;
        }
        CO_RETURN;
    }

    CORO_TASK(bool) CoroSetUp()
    {
        this->start_telemetry_test();

        this->root_service_ = std::make_shared<rpc::root_service>("host",
            rpc::DEFAULT_PREFIX
#ifdef CANOPY_BUILD_COROUTINE
            ,
            this->io_scheduler_
#endif
        );
        current_host_service = this->root_service_;

        rpc::shared_ptr<yyy::i_host> hst(new host());
        this->local_host_ptr_ = hst;

        auto child_transport = std::make_shared<rpc::local::child_transport>("main child", this->root_service_);
        child_transport->template set_child_entry_point<yyy::i_host, yyy::i_example>(
            // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
            [&](const rpc::shared_ptr<yyy::i_host>& host,
                rpc::shared_ptr<yyy::i_example>& new_example,
                const std::shared_ptr<rpc::child_service>& child_service_ptr) -> CORO_TASK(int)
            {
                this->i_host_ptr_ = host;
                child_service_ = child_service_ptr;
                child_service_weak_ = child_service_ptr;
                new_example = rpc::shared_ptr<yyy::i_example>(new marshalled_tests::example(child_service_ptr, nullptr));
                if (this->use_host_in_child_)
                    CO_AWAIT new_example->set_host(host);
                CO_RETURN rpc::error::OK();
            });

        auto ret = CO_AWAIT this->root_service_->connect_to_zone("main child", child_transport, hst, this->i_example_ptr_);
        startup_complete_ = true;
        if (ret != rpc::error::OK())
        {
            CO_RETURN false;
        }
        CO_RETURN true;
    }

    virtual void set_up()
    {
#ifdef CANOPY_BUILD_COROUTINE
        this->io_scheduler_ = std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(coro::scheduler::options{
            .thread_strategy = coro::scheduler::thread_strategy_t::manual, .pool = coro::thread_pool::options {
                .thread_count = 1,
            }}));

        RPC_ASSERT(this->io_scheduler_->spawn_detached(check_for_error(CoroSetUp())));
        while (startup_complete_ == false)
        {
            this->io_scheduler_->process_events(std::chrono::milliseconds(1));
        }
        while (this->io_scheduler_->process_events(std::chrono::milliseconds(1)) > 0)
        {
            // Keep processing while there are scheduled tasks
        }
#else
        check_for_error(CoroSetUp());
        ASSERT_EQ(startup_complete_, true);
#endif

        ASSERT_EQ(this->error_has_occurred_, false);
    }

    CORO_TASK(void) CoroTearDown()
    {
        child_service_ = nullptr;
        this->i_host_ptr_ = nullptr;
        this->i_example_ptr_ = nullptr;
        this->root_service_ = nullptr;
        current_host_service.reset();
        shutdown_complete_ = true;
        CO_RETURN;
    }

    virtual void tear_down()
    {
#ifdef CANOPY_BUILD_COROUTINE
        RPC_ASSERT(this->io_scheduler_->spawn_detached(CoroTearDown()));
        while (shutdown_complete_ == false)
        {
            this->io_scheduler_->process_events(std::chrono::milliseconds(1));
        }
        while (this->io_scheduler_->process_events(std::chrono::milliseconds(1)) > 0)
        {
            // Keep processing while there are scheduled tasks
        }
#else
        CoroTearDown();
#endif

        this->reset_telemetry_for_test();
    }

    CORO_TASK(rpc::shared_ptr<yyy::i_example>) create_new_zone()
    {
        rpc::shared_ptr<yyy::i_host> hst;
        if (this->use_host_in_child_)
            hst = this->local_host_ptr_.lock();

        rpc::shared_ptr<yyy::i_example> example_relay_ptr;

        auto child_transport = std::make_shared<rpc::local::child_transport>("main child", this->root_service_);
        child_transport->template set_child_entry_point<yyy::i_host, yyy::i_example>(
            // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
            [&](const rpc::shared_ptr<yyy::i_host>& host,
                rpc::shared_ptr<yyy::i_example>& new_example,
                const std::shared_ptr<rpc::child_service>& child_service_ptr) -> CORO_TASK(int)
            {
                new_example = rpc::shared_ptr<yyy::i_example>(new marshalled_tests::example(child_service_ptr, nullptr));
                if (this->use_host_in_child_)
                    CO_AWAIT new_example->set_host(host);
                CO_RETURN rpc::error::OK();
            });

        auto err_code
            = CO_AWAIT this->root_service_->connect_to_zone("main child", child_transport, hst, example_relay_ptr);

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
