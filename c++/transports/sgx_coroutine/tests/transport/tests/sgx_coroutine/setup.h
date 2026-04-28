/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#ifdef CANOPY_BUILD_ENCLAVE

#  include "test_host.h"
#  include "test_globals.h"
#  include <common/foo_impl.h>
#  include <common/tests.h>
#  include <gtest/gtest.h>
#  include <transports/sgx_coroutine/enclave/transport.h>

#  ifdef CANOPY_USE_TELEMETRY
#    include <rpc/telemetry/telemetry_service_factory.h>
#  endif

template<bool UseHostInChild, bool RunStandardTests, bool CreateNewZoneThenCreateSubordinatedZone>
class sgx_coroutine_setup
{
    std::shared_ptr<rpc::root_service> root_service_;
    std::shared_ptr<coro::scheduler> io_scheduler_;
    rpc::shared_ptr<yyy::i_host> i_host_ptr_;
    rpc::weak_ptr<yyy::i_host> local_host_ptr_;
    rpc::shared_ptr<yyy::i_example> i_example_ptr_;

    const bool has_enclave_ = true;
    bool use_host_in_child_ = UseHostInChild;
    bool run_standard_tests_ = RunStandardTests;
    bool error_has_occurred_ = false;
    bool shutdown_complete_ = false;

public:
    virtual ~sgx_coroutine_setup() = default;

    std::shared_ptr<rpc::root_service> get_root_service() const { return root_service_; }
    std::shared_ptr<coro::scheduler> get_scheduler() const { return io_scheduler_; }
    bool get_has_enclave() const { return has_enclave_; }
    bool is_sgx_setup() const { return true; }
    rpc::shared_ptr<yyy::i_host> get_local_host_ptr() { return local_host_ptr_.lock(); }
    rpc::shared_ptr<yyy::i_example> get_example() const { return i_example_ptr_; }
    void set_example(const rpc::shared_ptr<yyy::i_example>& example) { i_example_ptr_ = example; }
    rpc::shared_ptr<yyy::i_host> get_host() const { return i_host_ptr_; }
    void set_host(const rpc::shared_ptr<yyy::i_host>& host) { i_host_ptr_ = host; }
    bool get_use_host_in_child() const { return use_host_in_child_; }
    bool error_has_occurred() const { return error_has_occurred_; }
    bool has_service() { return true; }

    CORO_TASK(void) check_for_error(CORO_TASK(bool) task)
    {
        auto ret = CO_AWAIT task;
        if (!ret)
        {
            error_has_occurred_ = true;
        }
        CO_RETURN;
    }

    void set_up()
    {
#  ifdef CANOPY_USE_TELEMETRY
        auto test_info = ::testing::UnitTest::GetInstance()->current_test_info();
        rpc::telemetry::start_telemetry_test(
            rpc::telemetry::get_telemetry_service(), test_info->test_suite_name(), test_info->name());
#  endif

        io_scheduler_ = std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
            coro::scheduler::options{
                .thread_strategy = coro::scheduler::thread_strategy_t::manual,
                .pool = coro::thread_pool::options{.thread_count = 1},
            }));
        root_service_ = rpc::root_service::create("host", rpc::DEFAULT_PREFIX, io_scheduler_);
        current_host_service = root_service_;

        i_host_ptr_ = rpc::shared_ptr<yyy::i_host>(new host());
        local_host_ptr_ = i_host_ptr_;

        auto host_ptr = use_host_in_child_ ? i_host_ptr_ : nullptr;
        auto transport
            = std::make_shared<rpc::sgx::coro::enclave::transport>("main child", root_service_, coroutine_enclave_path);
        auto result
            = SYNC_WAIT((root_service_->connect_to_zone<yyy::i_host, yyy::i_example>("main child", transport, host_ptr)));

        i_example_ptr_ = std::move(result.output_interface);
        RPC_ASSERT(result.error_code == rpc::error::OK());
    }

    CORO_TASK(void) CoroTearDown()
    {
        this->i_host_ptr_ = nullptr;
        this->i_example_ptr_ = nullptr;
        this->root_service_ = nullptr;
        current_host_service.reset();
        shutdown_complete_ = true;
        CO_RETURN;
    }

    void tear_down()
    {
        shutdown_complete_ = false;
        RPC_ASSERT(io_scheduler_->spawn_detached(CoroTearDown()));
        while (!shutdown_complete_)
        {
            io_scheduler_->process_events(std::chrono::milliseconds(1));
        }
        for (int idle_iterations = 0; idle_iterations < 10;)
        {
            if (io_scheduler_->process_events(std::chrono::milliseconds(1)) == 0)
                ++idle_iterations;
            else
                idle_iterations = 0;
        }
        io_scheduler_ = nullptr;
#  ifdef CANOPY_USE_TELEMETRY
        rpc::telemetry::reset_telemetry_for_test(rpc::telemetry::get_telemetry_service());
#  endif
    }

    CORO_TASK(rpc::shared_ptr<yyy::i_example>) create_new_zone()
    {
        rpc::shared_ptr<yyy::i_example> ptr;
        rpc::get_new_zone_id_params zone_params{};
        zone_params.protocol_version = rpc::get_version();
        auto zone_result = CO_AWAIT root_service_->get_new_zone_id(zone_params);
        if (zone_result.error_code != rpc::error::OK())
            CO_RETURN nullptr;

        auto transport
            = std::make_shared<rpc::sgx::coro::enclave::transport>("main child", root_service_, coroutine_enclave_path);
        transport->set_adjacent_zone_id(zone_result.zone_id);
        auto result = CO_AWAIT root_service_->connect_to_zone<yyy::i_host, yyy::i_example>(
            "main child", transport, use_host_in_child_ ? i_host_ptr_ : nullptr);

        ptr = std::move(result.output_interface);
        if (result.error_code != rpc::error::OK())
            CO_RETURN nullptr;
        if (CreateNewZoneThenCreateSubordinatedZone)
        {
            rpc::shared_ptr<yyy::i_example> new_ptr;
            auto err_code
                = CO_AWAIT ptr->create_example_in_subordinate_zone(new_ptr, use_host_in_child_ ? i_host_ptr_ : nullptr);
            if (err_code != rpc::error::OK())
                CO_RETURN nullptr;
            ptr = new_ptr;
        }
        CO_RETURN ptr;
    }
};

#endif
