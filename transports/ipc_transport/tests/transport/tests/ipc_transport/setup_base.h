/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#ifdef CANOPY_BUILD_COROUTINE

#  include "test_globals.h"
#  include "test_host.h"

#  include <gtest/gtest.h>

#  include <common/tests.h>
#  include <common/transport_setup_base.h>
#  include <transports/ipc_transport/transport.h>

#  ifndef CANOPY_TEST_LIBCORO_SPSC_DLL_PATH
#    error "CANOPY_TEST_LIBCORO_SPSC_DLL_PATH must be defined"
#  endif

#  ifndef CANOPY_TEST_IPC_CHILD_HOST_PROCESS_PATH
#    error "CANOPY_TEST_IPC_CHILD_HOST_PROCESS_PATH must be defined"
#  endif

#  ifndef CANOPY_TEST_IPC_CHILD_PROCESS_PATH
#    error "CANOPY_TEST_IPC_CHILD_PROCESS_PATH must be defined"
#  endif

template<bool UseHostInChild, bool RunStandardTests, bool CreateNewZoneThenCreateSubordinatedZone>
class ipc_transport_setup_base
    : public transport_setup_base<UseHostInChild, RunStandardTests, CreateNewZoneThenCreateSubordinatedZone>
{
protected:
    struct isolated_child_process
    {
        rpc::zone dll_zone;
        std::shared_ptr<rpc::ipc_transport::transport> transport;
        rpc::shared_ptr<yyy::i_example> example;
    };

    rpc::zone host_zone_ = rpc::DEFAULT_PREFIX;
    rpc::zone dll_zone_ = make_dll_zone(1);

    std::shared_ptr<rpc::ipc_transport::transport> client_transport_;
    int startup_count_ = 0;

    static rpc::zone make_dll_zone(uint64_t offset)
    {
        auto address = rpc::DEFAULT_PREFIX;
        [[maybe_unused]] auto ok = address.set_subnet(address.get_subnet() + offset);
        RPC_ASSERT(ok);
        return rpc::zone(address);
    }

    CORO_TASK(bool)
    connect_child(
        std::string child_name,
        rpc::zone dll_zone,
        std::shared_ptr<rpc::ipc_transport::transport>& transport,
        rpc::shared_ptr<yyy::i_example>& example)
    {
        transport = rpc::ipc_transport::make_client(
            child_name,
            this->root_service_,
            rpc::ipc_transport::options{
                .process_executable = CANOPY_TEST_IPC_CHILD_HOST_PROCESS_PATH,
                .dll_path = CANOPY_TEST_LIBCORO_SPSC_DLL_PATH,
                .dll_zone = dll_zone,
                .kill_child_on_parent_death = true,
            });

        auto connect_result = CO_AWAIT this->root_service_->template connect_to_zone<yyy::i_host, yyy::i_example>(
            child_name.c_str(), transport, this->local_host_ptr_.lock());
        example = std::move(connect_result.output_interface);
        ++startup_count_;
        CO_RETURN connect_result.error_code == rpc::error::OK();
    }

    CORO_TASK(bool)
    connect_direct_child(
        std::string child_name,
        rpc::zone child_zone,
        std::shared_ptr<rpc::ipc_transport::transport>& transport,
        rpc::shared_ptr<yyy::i_example>& example)
    {
        transport = rpc::ipc_transport::make_client(
            child_name,
            this->root_service_,
            rpc::ipc_transport::options{
                .process_executable = CANOPY_TEST_IPC_CHILD_PROCESS_PATH,
                .dll_path = {},
                .dll_zone = child_zone,
                .process_kind = rpc::ipc_transport::child_process_kind::direct_service,
                .kill_child_on_parent_death = true,
            });

        auto connect_result = CO_AWAIT this->root_service_->template connect_to_zone<yyy::i_host, yyy::i_example>(
            child_name.c_str(), transport, this->local_host_ptr_.lock());
        example = std::move(connect_result.output_interface);
        ++startup_count_;
        CO_RETURN connect_result.error_code == rpc::error::OK();
    }

    void initialise_root_service()
    {
        this->root_service_ = std::make_shared<rpc::root_service>("host", host_zone_, this->io_scheduler_);
        current_host_service = this->root_service_;

        rpc::shared_ptr<yyy::i_host> hst(new host());
        this->i_host_ptr_ = hst;
        this->local_host_ptr_ = hst;
    }

    void destroy_isolated_child(isolated_child_process& child)
    {
        RPC_INFO("destroy_isolated_child: zone={} begin", child.dll_zone.get_subnet());
        child.transport.reset();
        child.example = nullptr;
        RPC_INFO("destroy_isolated_child: zone={} complete", child.dll_zone.get_subnet());
    }

public:
    void set_up_scheduler()
    {
        this->io_scheduler_ = std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
            coro::scheduler::options{.thread_strategy = coro::scheduler::thread_strategy_t::manual,
                .pool = coro::thread_pool::options{.thread_count = 1}}));
    }

    void pump_until_startup()
    {
        while (startup_count_ == 0)
            this->io_scheduler_->process_events(std::chrono::milliseconds(1));
    }

    void pump_until_startup_count(int expected_startup_count)
    {
        while (startup_count_ < expected_startup_count)
            this->io_scheduler_->process_events(std::chrono::milliseconds(1));
    }

    void common_teardown()
    {
        this->i_host_ptr_ = nullptr;
        this->i_example_ptr_ = nullptr;
        this->client_transport_.reset();

        if (this->io_scheduler_)
        {
            for (int idle_iterations = 0; idle_iterations < 10;)
            {
                if (this->io_scheduler_->process_events(std::chrono::milliseconds(1)) == 0)
                    ++idle_iterations;
                else
                    idle_iterations = 0;
            }
        }

        this->root_service_ = nullptr;
        current_host_service.reset();
        this->reset_telemetry_for_test();
    }
};

#endif // CANOPY_BUILD_COROUTINE
