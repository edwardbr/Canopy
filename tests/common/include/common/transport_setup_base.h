/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <common/foo_impl.h>

#ifdef CANOPY_USE_TELEMETRY
#include <rpc/telemetry/multiplexing_telemetry_service.h>
#endif

template<bool UseHostInChild, bool RunStandardTests, bool CreateNewZoneThenCreateSubordinatedZone>
class transport_setup_base
{
protected:
    std::shared_ptr<rpc::root_service> root_service_; // NOLINT(misc-non-private-member-variables-in-classes)

    rpc::shared_ptr<yyy::i_host> i_host_ptr_;       // NOLINT(misc-non-private-member-variables-in-classes)
    rpc::weak_ptr<yyy::i_host> local_host_ptr_;     // NOLINT(misc-non-private-member-variables-in-classes)
    rpc::shared_ptr<yyy::i_example> i_example_ptr_; // NOLINT(misc-non-private-member-variables-in-classes)

    const bool has_enclave_ = true;              // NOLINT(misc-non-private-member-variables-in-classes)
    bool use_host_in_child_ = UseHostInChild;    // NOLINT(misc-non-private-member-variables-in-classes)
    bool run_standard_tests_ = RunStandardTests; // NOLINT(misc-non-private-member-variables-in-classes)
    bool error_has_occurred_ = false;            // NOLINT(misc-non-private-member-variables-in-classes)

#ifdef CANOPY_BUILD_COROUTINE
    std::shared_ptr<coro::scheduler> io_scheduler_; // NOLINT(misc-non-private-member-variables-in-classes)
#endif

    void start_telemetry_test()
    {
#ifdef CANOPY_USE_TELEMETRY
        auto test_info = ::testing::UnitTest::GetInstance()->current_test_info();
        if (auto telemetry_service
            = std::static_pointer_cast<rpc::multiplexing_telemetry_service>(rpc::get_telemetry_service()))
        {
            telemetry_service->start_test(test_info->test_suite_name(), test_info->name());
        }
#endif
    }

    void reset_telemetry_for_test()
    {
#ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service
            = std::static_pointer_cast<rpc::multiplexing_telemetry_service>(rpc::get_telemetry_service()))
        {
            telemetry_service->reset_for_test();
        }
#endif
    }

public:
    virtual ~transport_setup_base() = default;

#ifdef CANOPY_BUILD_COROUTINE
    std::shared_ptr<coro::scheduler> get_scheduler() const { return io_scheduler_; }
#endif
    bool error_has_occurred() const { return error_has_occurred_; }
    bool has_service() { return true; }

    std::shared_ptr<rpc::root_service> get_root_service() const { return root_service_; }
    bool get_has_enclave() const { return has_enclave_; }
    bool is_sgx_setup() const { return false; }
    rpc::shared_ptr<yyy::i_example> get_example() const { return i_example_ptr_; }
    void set_example(const rpc::shared_ptr<yyy::i_example>& example) { i_example_ptr_ = example; }
    rpc::shared_ptr<yyy::i_host> get_host() const { return i_host_ptr_; }
    void set_host(const rpc::shared_ptr<yyy::i_host>& host) { i_host_ptr_ = host; }
    rpc::shared_ptr<yyy::i_host> get_local_host_ptr() { return local_host_ptr_.lock(); }
    bool get_use_host_in_child() const { return use_host_in_child_; }

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
