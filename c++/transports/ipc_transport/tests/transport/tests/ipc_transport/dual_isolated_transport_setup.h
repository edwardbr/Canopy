/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#ifdef CANOPY_BUILD_COROUTINE

#  include <transport/tests/ipc_transport/setup_base.h>

template<bool UseHostInChild, bool RunStandardTests, bool CreateNewZoneThenCreateSubordinatedZone>
class ipc_child_host_process_dual_setup
    : public ipc_transport_setup_base<UseHostInChild, RunStandardTests, CreateNewZoneThenCreateSubordinatedZone>
{
    using base_type = ipc_transport_setup_base<UseHostInChild, RunStandardTests, CreateNewZoneThenCreateSubordinatedZone>;

    typename base_type::isolated_child_process first_child_;
    typename base_type::isolated_child_process second_child_;
    std::shared_ptr<rpc::ipc_transport::transport> second_client_transport_;
    rpc::shared_ptr<yyy::i_example> second_example_ptr_;

public:
    [[nodiscard]] rpc::shared_ptr<yyy::i_example> get_peer_example() const { return second_example_ptr_; }
    void set_peer_example(const rpc::shared_ptr<yyy::i_example>& example) { second_example_ptr_ = example; }

    void set_up()
    {
        this->start_telemetry_test();
        this->set_up_scheduler();
        this->initialise_root_service();

        first_child_.dll_zone = this->dll_zone_;
        second_child_.dll_zone = this->make_dll_zone(2);

        RPC_ASSERT(this->io_scheduler_->spawn_detached(this->check_for_error(
            this->connect_child("ipc child a", first_child_.dll_zone, this->client_transport_, this->i_example_ptr_))));
        RPC_ASSERT(this->io_scheduler_->spawn_detached(this->check_for_error(
            this->connect_child("ipc child b", second_child_.dll_zone, second_client_transport_, second_example_ptr_))));
        this->pump_until_startup_count(2);
        ASSERT_EQ(this->error_has_occurred_, false);
    }

    void tear_down()
    {
        RPC_INFO("dual_isolated tear_down begin");
        second_example_ptr_ = nullptr;

        // Pump until the second child's streaming transport reaches DISCONNECTED.
        // pump_until_idle() does not work here — see pump_until_disconnected() comment.
        this->pump_until_disconnected(second_client_transport_);

        second_client_transport_.reset();

        // The first transport is still connected here, so pump_until_idle() would never
        // converge: its receive loop keeps the manual scheduler busy with 1ms polling.
        // A short bounded pump is enough to let the second transport's cleanup run.
        this->pump_for(std::chrono::milliseconds(20));

        this->common_teardown();
        this->destroy_isolated_child(second_child_);
        this->destroy_isolated_child(first_child_);
        RPC_INFO("dual_isolated tear_down complete");
    }
};

#endif // CANOPY_BUILD_COROUTINE
