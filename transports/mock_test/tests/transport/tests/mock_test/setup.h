/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <memory>
#include <gtest/gtest.h>
#include <rpc/rpc.h>
#include <rpc/internal/pass_through.h>
#include <transports/mock_test/transport.h>

#ifdef CANOPY_BUILD_COROUTINE
#include <coro/io_scheduler.hpp>
#endif

// Setup class for passthrough tests - handles both blocking and coroutine modes
class passthrough_setup
{
#ifdef CANOPY_BUILD_COROUTINE
    std::shared_ptr<coro::io_scheduler> io_scheduler_;
#endif
    std::shared_ptr<rpc::service> service_;
    std::shared_ptr<rpc::mock_test::mock_transport> forward_transport_;
    std::shared_ptr<rpc::mock_test::mock_transport> reverse_transport_;
    std::shared_ptr<rpc::pass_through> passthrough_;
    rpc::destination_zone forward_dest_;
    rpc::destination_zone reverse_dest_;

    bool error_has_occured_ = false;
    bool startup_complete_ = false;
    bool shutdown_complete_ = false;

public:
#ifdef CANOPY_BUILD_COROUTINE
    std::shared_ptr<coro::io_scheduler> get_scheduler() const { return io_scheduler_; }
#endif
    bool error_has_occured() const { return error_has_occured_; }
    std::shared_ptr<rpc::service> get_service() const { return service_; }
    std::shared_ptr<rpc::mock_test::mock_transport> get_forward_transport() const { return forward_transport_; }
    std::shared_ptr<rpc::mock_test::mock_transport> get_reverse_transport() const { return reverse_transport_; }
    std::shared_ptr<rpc::pass_through> get_passthrough() const { return passthrough_; }
    rpc::destination_zone get_forward_dest() const { return forward_dest_; }
    rpc::destination_zone get_reverse_dest() const { return reverse_dest_; }

    CORO_TASK(void) check_for_error(CORO_TASK(bool) task)
    {
        auto ret = CO_AWAIT task;
        if (!ret)
        {
            error_has_occured_ = true;
        }
        CO_RETURN;
    }

    CORO_TASK(bool) CoroSetUp()
    {
        RPC_INFO("passthrough_setup::CoroSetUp - Starting setup");
        RPC_INFO("passthrough_setup::CoroSetUp - Creating service");
#ifdef CANOPY_BUILD_COROUTINE
        service_ = std::make_shared<rpc::service>("test_service", rpc::zone{1}, io_scheduler_);
#else
        service_ = std::make_shared<rpc::service>("test_service", rpc::zone{1});
#endif

        // Create mock transports
        RPC_INFO("passthrough_setup::CoroSetUp - Setting up destinations");
        forward_dest_ = rpc::destination_zone{100};
        reverse_dest_ = rpc::destination_zone{200};

        RPC_INFO("passthrough_setup::CoroSetUp - Creating forward transport");
        forward_transport_ = std::make_shared<rpc::mock_test::mock_transport>("forward", service_, rpc::zone{100});
        RPC_INFO("passthrough_setup::CoroSetUp - Creating reverse transport");
        reverse_transport_ = std::make_shared<rpc::mock_test::mock_transport>("reverse", service_, rpc::zone{200});

        // Register transports with service so it knows about them
        // This is needed for add_ref_happy_path test and proper transport state handling
        RPC_INFO("passthrough_setup::CoroSetUp - Registering transports with service");
        service_->add_transport(forward_dest_, forward_transport_);
        service_->add_transport(reverse_dest_, reverse_transport_);

        // Test edit to see if I'm out of plan mode
        RPC_INFO("passthrough_setup::CoroSetUp - Creating passthrough");
        passthrough_ = std::static_pointer_cast<rpc::pass_through>(rpc::transport::create_pass_through(
            forward_transport_, reverse_transport_, service_, forward_dest_, reverse_dest_));

        RPC_INFO("passthrough_setup::CoroSetUp - Setup complete");
        startup_complete_ = true;
        CO_RETURN true;
    }

    virtual void set_up()
    {
        RPC_INFO("passthrough_setup::set_up - Starting");
#ifdef CANOPY_BUILD_COROUTINE
        RPC_INFO("passthrough_setup::set_up - CANOPY_BUILD_COROUTINE mode");
        RPC_INFO("passthrough_setup::set_up - Creating io_scheduler");
        io_scheduler_ = coro::io_scheduler::make_shared(
            coro::io_scheduler::options{.thread_strategy = coro::io_scheduler::thread_strategy_t::manual,
                .pool = coro::thread_pool::options{
                    .thread_count = 1,
                }});
        RPC_INFO("passthrough_setup::set_up - Spawning CoroSetUp");
        RPC_ASSERT(io_scheduler_->spawn(check_for_error(CoroSetUp())));
        RPC_INFO("passthrough_setup::set_up - Processing events until startup complete");
        while (startup_complete_ == false)
        {
            io_scheduler_->process_events(std::chrono::milliseconds(1));
        }
        while (io_scheduler_->process_events(std::chrono::milliseconds(1)) > 0)
        {
            // Keep processing while there are scheduled tasks
        }
        RPC_INFO("passthrough_setup::set_up - Event processing complete");
#else
        RPC_INFO("passthrough_setup::set_up - Non-coroutine mode");
        check_for_error(CoroSetUp());
        ASSERT_EQ(startup_complete_, true);
#endif
        RPC_INFO("passthrough_setup::set_up - Checking for errors");
        ASSERT_EQ(error_has_occured_, false);
        RPC_INFO("passthrough_setup::set_up - Complete");
    }

    CORO_TASK(void) CoroTearDown()
    {
        RPC_INFO("passthrough_setup::CoroTearDown - Starting cleanup");
        // Clean up in reverse order
        passthrough_.reset();
        RPC_INFO("passthrough_setup::CoroTearDown - Passthrough reset");
        forward_transport_.reset();
        RPC_INFO("passthrough_setup::CoroTearDown - Forward transport reset");
        reverse_transport_.reset();
        RPC_INFO("passthrough_setup::CoroTearDown - Reverse transport reset");
        service_.reset();
        RPC_INFO("passthrough_setup::CoroTearDown - Service reset");

        shutdown_complete_ = true;
        RPC_INFO("passthrough_setup::CoroTearDown - Complete");
        CO_RETURN;
    }

    virtual void tear_down()
    {
#ifdef CANOPY_BUILD_COROUTINE
        RPC_ASSERT(io_scheduler_->spawn(CoroTearDown()));
        while (shutdown_complete_ == false)
        {
            io_scheduler_->process_events(std::chrono::milliseconds(1));
        }
        while (io_scheduler_->process_events(std::chrono::milliseconds(1)) > 0)
        {
        }
#else
        CoroTearDown();
#endif
    }
};
