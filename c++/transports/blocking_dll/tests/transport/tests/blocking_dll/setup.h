/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#ifndef CANOPY_BUILD_COROUTINE

#  include "test_host.h"
#  include "test_globals.h"
#  include <gtest/gtest.h>

#  include <rpc/rpc.h>
#  include <common/tests.h>
#  include <common/transport_setup_base.h>
#  include <transports/blocking_dll/factory.h>

#  ifndef CANOPY_TEST_DLL_PATH
#    error "CANOPY_TEST_DLL_PATH must be defined (set by CMake to the path of canopy_example_blocking_dll)"
#  endif

template<bool UseHostInChild, bool RunStandardTests, bool CreateNewZoneThenCreateSubordinatedZone>
class blocking_dll_transport_setup
    : public transport_setup_base<UseHostInChild, RunStandardTests, CreateNewZoneThenCreateSubordinatedZone>
{
    bool startup_complete_ = false;
    bool shutdown_complete_ = false;

public:
    ~blocking_dll_transport_setup() override = default;

    CORO_TASK(bool) CoroSetUp()
    {
        this->start_telemetry_test();

        this->root_service_ = rpc::root_service::create("host", rpc::DEFAULT_PREFIX);
        current_host_service = this->root_service_;

        rpc::shared_ptr<yyy::i_host> hst(new host());
        this->local_host_ptr_ = hst;

        rpc::blocking_dll::transport_settings settings;
        settings.name = "main child";
        settings.dynamic_library_path = CANOPY_TEST_DLL_PATH;

        auto connect_result
            = CO_AWAIT rpc::blocking_dll::connect_rpc<yyy::i_host, yyy::i_example>(settings, hst, this->root_service_);
        this->i_example_ptr_ = std::move(connect_result.output_interface);
        startup_complete_ = true;
        if (connect_result.error_code != rpc::error::OK())
        {
            CO_RETURN false;
        }
        CO_RETURN true;
    }

    virtual void set_up()
    {
        this->check_for_error(CoroSetUp());
        ASSERT_EQ(startup_complete_, true);
        ASSERT_EQ(this->error_has_occurred_, false);
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

    virtual void tear_down()
    {
        CoroTearDown();
        this->reset_telemetry_for_test();
    }

    CORO_TASK(rpc::shared_ptr<yyy::i_example>) create_new_zone()
    {
        rpc::shared_ptr<yyy::i_host> hst;
        if (this->use_host_in_child_)
            hst = this->local_host_ptr_.lock();

        rpc::blocking_dll::transport_settings settings;
        settings.name = "new child";
        settings.dynamic_library_path = CANOPY_TEST_DLL_PATH;

        auto connect_result
            = CO_AWAIT rpc::blocking_dll::connect_rpc<yyy::i_host, yyy::i_example>(settings, hst, this->root_service_);
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

#endif // !CANOPY_BUILD_COROUTINE
