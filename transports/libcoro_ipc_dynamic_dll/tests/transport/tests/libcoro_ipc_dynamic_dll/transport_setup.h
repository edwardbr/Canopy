/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#ifdef CANOPY_BUILD_COROUTINE

#  include <transport/tests/libcoro_ipc_dynamic_dll/setup_base.h>

template<bool UseHostInChild, bool RunStandardTests, bool CreateNewZoneThenCreateSubordinatedZone>
class libcoro_ipc_dll_transport_setup
    : public libcoro_ipc_dll_setup_base<UseHostInChild, RunStandardTests, CreateNewZoneThenCreateSubordinatedZone>
{
    rpc::libcoro_ipc_dynamic_dll::queue_pair queues_{};
    std::shared_ptr<rpc::libcoro_ipc_dynamic_dll::loaded_library> loaded_;

public:
    void set_up()
    {
        this->start_telemetry_test();
        this->set_up_scheduler();
        this->initialise_root_service();

        loaded_ = rpc::libcoro_ipc_dynamic_dll::loaded_library::load(CANOPY_TEST_LIBCORO_IPC_DLL_PATH,
            "libcoro_ipc_dynamic_dll",
            this->dll_zone_,
            this->host_zone_,
            &queues_.dll_to_host,
            &queues_.host_to_dll);
        ASSERT_NE(loaded_, nullptr);

        RPC_ASSERT(this->io_scheduler_->spawn_detached(this->check_for_error(
            this->connect_child("ipc child", &queues_, this->client_transport_, this->i_example_ptr_))));
        this->pump_until_startup();
        ASSERT_EQ(this->error_has_occurred_, false);
    }

    void tear_down()
    {
        this->i_host_ptr_ = nullptr;
        this->i_example_ptr_ = nullptr;
        this->client_transport_.reset();

        for (int idle_iterations = 0; idle_iterations < 10;)
        {
            if (this->io_scheduler_->process_events(std::chrono::milliseconds(1)) == 0)
                ++idle_iterations;
            else
                idle_iterations = 0;
        }

        if (loaded_)
        {
            loaded_->stop();
            loaded_.reset();
        }

        this->root_service_ = nullptr;
        current_host_service.reset();
        this->reset_telemetry_for_test();
    }
};

#endif // CANOPY_BUILD_COROUTINE
