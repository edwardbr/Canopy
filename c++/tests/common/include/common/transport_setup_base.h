/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include "test_globals.h"

#include <atomic>
#include <chrono>
#include <common/foo_impl.h>

#ifdef CANOPY_USE_TELEMETRY
#  include <rpc/telemetry/telemetry_service_factory.h>
#endif

template<bool UseHostInChild, bool RunStandardTests, bool CreateNewZoneThenCreateSubordinatedZone>
class transport_setup_base
{
protected:
    std::shared_ptr<rpc::root_service> root_service_; // NOLINT(misc-non-private-member-variables-in-classes)

    rpc::shared_ptr<yyy::i_host> i_host_ptr_;       // NOLINT(misc-non-private-member-variables-in-classes)
    rpc::weak_ptr<yyy::i_host> local_host_ptr_;     // NOLINT(misc-non-private-member-variables-in-classes)
    rpc::shared_ptr<yyy::i_example> i_example_ptr_; // NOLINT(misc-non-private-member-variables-in-classes)

    bool use_host_in_child_ = UseHostInChild;    // NOLINT(misc-non-private-member-variables-in-classes)
    bool run_standard_tests_ = RunStandardTests; // NOLINT(misc-non-private-member-variables-in-classes)
    bool error_has_occurred_ = false;            // NOLINT(misc-non-private-member-variables-in-classes)

#ifdef CANOPY_BUILD_COROUTINE
    std::shared_ptr<coro::scheduler> io_scheduler_; // NOLINT(misc-non-private-member-variables-in-classes)
    std::atomic_bool teardown_interfaces_released_ = false;
    std::atomic_bool teardown_root_shutdown_complete_ = false;
#endif

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

#ifdef CANOPY_BUILD_COROUTINE
    std::shared_ptr<rpc::event> make_root_shutdown_event_for_test()
    {
        auto shutdown_event = std::make_shared<rpc::event>(false);
        if (root_service_)
            root_service_->set_shutdown_event(shutdown_event);
        return shutdown_event;
    }

    CORO_TASK(void) CoroReleaseInterfacesForTearDown()
    {
        i_example_ptr_ = nullptr;
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
#endif

public:
    virtual ~transport_setup_base() = default;

#ifdef CANOPY_BUILD_COROUTINE
    [[nodiscard]] std::shared_ptr<coro::scheduler> get_scheduler() const { return io_scheduler_; }
#endif
    [[nodiscard]] bool error_has_occurred() const { return error_has_occurred_; }
    bool has_service() { return true; }

    [[nodiscard]] std::shared_ptr<rpc::root_service> get_root_service() const { return root_service_; }
    [[nodiscard]] bool supports_process_local_reference_tests() const { return false; }
    [[nodiscard]] rpc::shared_ptr<yyy::i_example> get_example() const { return i_example_ptr_; }
    void set_example(const rpc::shared_ptr<yyy::i_example>& example) { i_example_ptr_ = example; }
    [[nodiscard]] rpc::shared_ptr<yyy::i_host> get_host() const { return i_host_ptr_; }
    void set_host(const rpc::shared_ptr<yyy::i_host>& host) { i_host_ptr_ = host; }
    rpc::shared_ptr<yyy::i_host> get_local_host_ptr() { return local_host_ptr_.lock(); }
    [[nodiscard]] bool get_use_host_in_child() const { return use_host_in_child_; }

    CORO_TASK(void) check_for_error(CORO_TASK(bool) task)
    {
        auto ret = CO_AWAIT task;
        if (!ret)
        {
            RPC_ASSERT(false);
            error_has_occurred_ = true;
        }
        CO_RETURN;
    }

    CORO_TASK(rpc::shared_ptr<yyy::i_example>) create_new_zone()
    {
        RPC_INFO("create_new_zone is not implemented for this setup");
        CO_RETURN nullptr;
    }
};
