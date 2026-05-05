/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include "sgx_coroutine_test_host.h"
#include "test_globals.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <vector>

#include <gtest/gtest.h>
#include <io_uring_test/test.h>
#include <rpc/rpc.h>
#include <transports/sgx_coroutine/enclave/transport.h>

#ifdef CANOPY_USE_TELEMETRY
#  include <rpc/telemetry/telemetry_service_factory.h>
#endif

class sgx_coroutine_test_setup
{
    std::shared_ptr<rpc::root_service> root_service_;
    std::shared_ptr<coro::scheduler> io_scheduler_;
    rpc::shared_ptr<yyy::i_host> i_host_ptr_;
    rpc::shared_ptr<io_uring_test::i_test_uring> i_test_uring_ptr_;
    std::vector<std::weak_ptr<rpc::sgx::coro::enclave::transport>> transports_;
    std::atomic_bool teardown_interfaces_released_ = false;
    std::atomic_bool teardown_root_shutdown_complete_ = false;

    [[nodiscard]] bool all_transports_expired() const
    {
        for (const auto& transport : transports_)
        {
            if (!transport.expired())
                return false;
        }
        return true;
    }

    void start_telemetry_test()
    {
#ifdef CANOPY_USE_TELEMETRY
        auto test_info = ::testing::UnitTest::GetInstance()->current_test_info();
        rpc::telemetry::start_telemetry_test(
            rpc::telemetry::get_telemetry_service(), test_info->test_suite_name(), test_info->name());
#endif
    }

    void reset_telemetry_for_test()
    {
#ifdef CANOPY_USE_TELEMETRY
        rpc::telemetry::reset_telemetry_for_test(rpc::telemetry::get_telemetry_service());
#endif
    }

    std::shared_ptr<rpc::event> make_root_shutdown_event_for_test()
    {
        auto shutdown_event = std::make_shared<rpc::event>(false);
#ifdef FOR_SGX
        shutdown_event->set_scheduler(io_scheduler_.get());
#endif
        if (root_service_)
            root_service_->set_shutdown_event(shutdown_event);
        return shutdown_event;
    }

    CORO_TASK(void) CoroReleaseInterfacesForTearDown()
    {
        i_test_uring_ptr_ = nullptr;
        i_host_ptr_ = nullptr;
        teardown_interfaces_released_.store(true);
        CO_RETURN;
    }

    CORO_TASK(void) CoroResetRootServiceForTearDown(std::shared_ptr<rpc::event> shutdown_event)
    {
        root_service_ = nullptr;
        current_host_service.reset();
        if (shutdown_event)
            CO_AWAIT shutdown_event->wait();
        teardown_root_shutdown_complete_.store(true);
        CO_RETURN;
    }

    void release_interfaces_and_root_service_for_test(const std::shared_ptr<rpc::event>& shutdown_event)
    {
        teardown_interfaces_released_.store(false);
        RPC_ASSERT(io_scheduler_->spawn_detached(CoroReleaseInterfacesForTearDown()));
        while (!teardown_interfaces_released_.load())
        {
            io_scheduler_->process_events(std::chrono::milliseconds(1));
        }

        teardown_root_shutdown_complete_.store(false);
        RPC_ASSERT(io_scheduler_->spawn_detached(CoroResetRootServiceForTearDown(shutdown_event)));
        while (!teardown_root_shutdown_complete_.load())
        {
            io_scheduler_->process_events(std::chrono::milliseconds(1));
        }

        RPC_ASSERT(!shutdown_event || shutdown_event->is_set());
    }

public:
    ~sgx_coroutine_test_setup() = default;

    bool is_sgx_setup() const { return true; }

    void set_up(rpc::io_uring::host_controller::options host_controller_options = {})
    {
        start_telemetry_test();

        io_scheduler_ = std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
            coro::scheduler::options{
                .thread_strategy = coro::scheduler::thread_strategy_t::manual,
                .pool = coro::thread_pool::options{.thread_count = 1},
            }));
        root_service_ = rpc::root_service::create("sgx coroutine test host", rpc::DEFAULT_PREFIX, io_scheduler_);
        current_host_service = root_service_;

        auto host_result = host_with_io_uring_control::create_for_test(io_scheduler_, host_controller_options);
        RPC_ASSERT(host_result.error_code == rpc::error::OK());
        i_host_ptr_ = std::move(host_result.output_interface);

        auto host_ptr = i_host_ptr_;
        auto transport = std::make_shared<rpc::sgx::coro::enclave::transport>(
            "main child", root_service_, sgx_coroutine_test_enclave_path);
        transports_.push_back(transport);
        auto result = SYNC_WAIT((root_service_->template connect_to_zone<yyy::i_host, io_uring_test::i_test_uring>(
            "main child", transport, host_ptr)));

        i_test_uring_ptr_ = std::move(result.output_interface);
        RPC_ASSERT(result.error_code == rpc::error::OK());
    }

    void tear_down()
    {
        auto scheduler = io_scheduler_;

        auto service_shutdown_event = this->make_root_shutdown_event_for_test();
        release_interfaces_and_root_service_for_test(service_shutdown_event);

        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{5000};
        while (!all_transports_expired() && std::chrono::steady_clock::now() < deadline)
        {
            scheduler->process_events(std::chrono::milliseconds{1});
        }
        RPC_ASSERT(all_transports_expired());
        transports_.clear();

        scheduler->shutdown();

        io_scheduler_ = nullptr;
        scheduler.reset();
        reset_telemetry_for_test();
    }

    [[nodiscard]] rpc::shared_ptr<yyy::i_host> get_host() const { return i_host_ptr_; }
    [[nodiscard]] rpc::shared_ptr<io_uring_test::i_test_uring> get_test_uring() const { return i_test_uring_ptr_; }
    [[nodiscard]] std::shared_ptr<rpc::root_service> get_root_service() const { return root_service_; }
    [[nodiscard]] std::shared_ptr<coro::scheduler> get_scheduler() const { return io_scheduler_; }

    CORO_TASK(rpc::shared_ptr<io_uring_test::i_test_uring>) create_new_zone()
    {
        rpc::shared_ptr<io_uring_test::i_test_uring> ptr;
        rpc::get_new_zone_id_params zone_params{};
        zone_params.protocol_version = rpc::get_version();
        auto zone_result = CO_AWAIT root_service_->get_new_zone_id(zone_params);
        if (zone_result.error_code != rpc::error::OK())
            CO_RETURN nullptr;

        auto transport = std::make_shared<rpc::sgx::coro::enclave::transport>(
            "main child", root_service_, sgx_coroutine_test_enclave_path);
        transports_.push_back(transport);
        transport->set_adjacent_zone_id(zone_result.zone_id);
        auto result = CO_AWAIT root_service_->template connect_to_zone<yyy::i_host, io_uring_test::i_test_uring>(
            "main child", transport, i_host_ptr_);

        ptr = std::move(result.output_interface);
        if (result.error_code != rpc::error::OK())
            CO_RETURN nullptr;
        CO_RETURN ptr;
    }
};
