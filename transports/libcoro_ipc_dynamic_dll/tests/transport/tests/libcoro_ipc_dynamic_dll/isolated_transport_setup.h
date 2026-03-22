/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#ifdef CANOPY_BUILD_COROUTINE

#  include <transport/tests/libcoro_ipc_dynamic_dll/setup_base.h>

template<bool UseHostInChild, bool RunStandardTests, bool CreateNewZoneThenCreateSubordinatedZone>
class libcoro_ipc_dll_isolated_transport_setup
    : public libcoro_ipc_dll_setup_base<UseHostInChild, RunStandardTests, CreateNewZoneThenCreateSubordinatedZone>
{
    typename libcoro_ipc_dll_setup_base<UseHostInChild, RunStandardTests, CreateNewZoneThenCreateSubordinatedZone>::isolated_child_process
        child_;

public:
    void set_up()
    {
        this->start_telemetry_test();
        this->set_up_scheduler();
        this->initialise_root_service();
        child_.dll_zone = this->dll_zone_;
        this->create_mapped_file(child_, "/tmp/canopy_ipc_dll_XXXXXX");
        this->spawn_loader(child_);

        RPC_ASSERT(this->io_scheduler_->spawn_detached(this->check_for_error(
            this->connect_child("ipc child", child_.queues, this->client_transport_, this->i_example_ptr_))));
        this->pump_until_startup();
        ASSERT_EQ(this->error_has_occurred_, false);
    }

    void tear_down()
    {
        this->common_teardown();
        this->destroy_isolated_child(child_);
    }
};

#endif // CANOPY_BUILD_COROUTINE
