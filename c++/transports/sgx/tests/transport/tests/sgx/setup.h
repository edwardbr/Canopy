/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#ifdef CANOPY_BUILD_ENCLAVE

#  include <atomic>
#  include "test_host.h"
#  include "test_globals.h"
#  include <gtest/gtest.h>
#  include <common/foo_impl.h>
#  include <common/tests.h>
#  include <transports/sgx/transport.h>

#  ifdef CANOPY_USE_TELEMETRY
#    include <rpc/telemetry/i_telemetry_service.h>
#  endif

template<bool UseHostInChild, bool RunStandardTests, bool CreateNewZoneThenCreateSubordinatedZone> class sgx_setup
{
    std::shared_ptr<rpc::root_service> root_service_;
    rpc::shared_ptr<yyy::i_host> i_host_ptr_;
    rpc::weak_ptr<yyy::i_host> local_host_ptr_;
    rpc::shared_ptr<yyy::i_example> i_example_ptr_;

    const bool has_enclave_ = true;
    bool use_host_in_child_ = UseHostInChild;
    bool run_standard_tests_ = RunStandardTests;

    bool error_has_occurred_ = false;

public:
    virtual ~sgx_setup() = default;

    std::shared_ptr<rpc::root_service> get_root_service() const { return root_service_; }
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

    virtual void set_up()
    {
#  ifdef CANOPY_USE_TELEMETRY
        auto test_info = ::testing::UnitTest::GetInstance()->current_test_info();
        if (auto telemetry_service
            = std::static_pointer_cast<rpc::multiplexing_telemetry_service>(rpc::get_telemetry_service()))
        {
            telemetry_service->start_test(test_info->test_suite_name(), test_info->name());
        }
#  endif
        root_service_ = std::make_shared<rpc::root_service>("host", rpc::DEFAULT_PREFIX);
        current_host_service = root_service_;

        i_host_ptr_ = rpc::shared_ptr<yyy::i_host>(new host());
        local_host_ptr_ = i_host_ptr_;

        auto host_ptr = use_host_in_child_ ? i_host_ptr_ : nullptr;

        auto transport = std::make_shared<rpc::sgx::enclave_transport>("main child", root_service_, enclave_path);
        auto result
            = SYNC_WAIT((root_service_->connect_to_zone<yyy::i_host, yyy::i_example>("main child", transport, host_ptr)));

        i_example_ptr_ = std::move(result.output_interface);
        RPC_ASSERT(result.error_code == rpc::error::OK());
    }

    virtual void tear_down()
    {
        i_example_ptr_ = nullptr;
        i_host_ptr_ = nullptr;
        root_service_ = nullptr;
        current_host_service.reset();
#  ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service
            = std::static_pointer_cast<rpc::multiplexing_telemetry_service>(rpc::get_telemetry_service()))
        {
            telemetry_service->reset_for_test();
        }
#  endif
    }

    rpc::shared_ptr<yyy::i_example> create_new_zone()
    {
        rpc::shared_ptr<yyy::i_example> ptr;
        rpc::get_new_zone_id_params zone_params{};
        zone_params.protocol_version = rpc::get_version();
        auto zone_result = SYNC_WAIT(root_service_->get_new_zone_id(zone_params));
        if (zone_result.error_code != rpc::error::OK())
            return nullptr;

        auto transport = std::make_shared<rpc::sgx::enclave_transport>("main child", root_service_, enclave_path);
        transport->set_adjacent_zone_id(zone_result.zone_id);
        auto result = SYNC_WAIT((root_service_->connect_to_zone<yyy::i_host, yyy::i_example>(
            "main child", transport, use_host_in_child_ ? i_host_ptr_ : nullptr)));

        ptr = std::move(result.output_interface);
        if (result.error_code != rpc::error::OK())
            return nullptr;
        if (CreateNewZoneThenCreateSubordinatedZone)
        {
            rpc::shared_ptr<yyy::i_example> new_ptr;
            auto err_code = ptr->create_example_in_subordinate_zone(new_ptr, use_host_in_child_ ? i_host_ptr_ : nullptr);
            if (err_code != rpc::error::OK())
                return nullptr;
            ptr = new_ptr;
        }
        return ptr;
    }
};

#endif // CANOPY_BUILD_ENCLAVE
