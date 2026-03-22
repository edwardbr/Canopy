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
#  include <cstdio>
#  include <fcntl.h>
#  include <new>
#  include <spawn.h>
#  include <streaming/spsc_queue/stream.h>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <sys/wait.h>
#  include <thread>
#  include <transports/libcoro_ipc_dynamic_dll/loaded_library.h>
#  include <transports/libcoro_ipc_dynamic_dll/transport.h>
#  include <vector>

extern char** environ;

#  ifndef CANOPY_TEST_LIBCORO_IPC_DLL_PATH
#    error "CANOPY_TEST_LIBCORO_IPC_DLL_PATH must be defined"
#  endif

#  ifndef CANOPY_TEST_LIBCORO_IPC_LOADER_PATH
#    error "CANOPY_TEST_LIBCORO_IPC_LOADER_PATH must be defined"
#  endif

template<bool UseHostInChild, bool RunStandardTests, bool CreateNewZoneThenCreateSubordinatedZone>
class libcoro_ipc_dll_setup_base
    : public transport_setup_base<UseHostInChild, RunStandardTests, CreateNewZoneThenCreateSubordinatedZone>
{
protected:
    struct isolated_child_process
    {
        std::string mapped_file;
        rpc::libcoro_ipc_dynamic_dll::queue_pair* queues = nullptr;
        pid_t child_pid = -1;
        rpc::zone dll_zone;
        std::shared_ptr<rpc::stream_transport::transport> transport;
        rpc::shared_ptr<yyy::i_example> example;
    };

    rpc::zone host_zone_ = rpc::DEFAULT_PREFIX;
    rpc::zone dll_zone_ = make_dll_zone(1);

    std::shared_ptr<rpc::stream_transport::transport> client_transport_;
    int startup_count_ = 0;

    static rpc::zone make_dll_zone(uint64_t offset)
    {
        auto address = rpc::DEFAULT_PREFIX;
        [[maybe_unused]] bool ok = address.set_subnet(address.get_subnet() + offset);
        RPC_ASSERT(ok);
        return rpc::zone(address);
    }

    CORO_TASK(bool)
    connect_child(const std::string& child_name,
        rpc::libcoro_ipc_dynamic_dll::queue_pair* queues,
        std::shared_ptr<rpc::stream_transport::transport>& transport,
        rpc::shared_ptr<yyy::i_example>& example)
    {
        transport = rpc::libcoro_ipc_dynamic_dll::make_client(child_name, this->root_service_, queues);

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

    void create_mapped_file(isolated_child_process& child, const char* file_template)
    {
        char file_name[64];
        std::snprintf(file_name, sizeof(file_name), "%s", file_template);
        int fd = ::mkstemp(file_name);
        ASSERT_GE(fd, 0);
        child.mapped_file = file_name;
        ASSERT_EQ(::ftruncate(fd, sizeof(rpc::libcoro_ipc_dynamic_dll::queue_pair)), 0);
        child.queues = static_cast<rpc::libcoro_ipc_dynamic_dll::queue_pair*>(
            ::mmap(nullptr, sizeof(rpc::libcoro_ipc_dynamic_dll::queue_pair), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
        ::close(fd);
        ASSERT_NE(child.queues, MAP_FAILED);
        new (child.queues) rpc::libcoro_ipc_dynamic_dll::queue_pair{};
    }

    void spawn_loader(isolated_child_process& child)
    {
        auto dll_subnet = std::to_string(child.dll_zone.get_subnet());
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(CANOPY_TEST_LIBCORO_IPC_LOADER_PATH));
        argv.push_back(const_cast<char*>(CANOPY_TEST_LIBCORO_IPC_DLL_PATH));
        argv.push_back(child.mapped_file.data());
        argv.push_back(dll_subnet.data());
        argv.push_back(nullptr);
        ASSERT_EQ(
            ::posix_spawn(&child.child_pid, CANOPY_TEST_LIBCORO_IPC_LOADER_PATH, nullptr, nullptr, argv.data(), environ),
            0);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    void destroy_isolated_child(isolated_child_process& child)
    {
        RPC_INFO("destroy_isolated_child: zone={} pid={} begin", child.dll_zone.get_subnet(), child.child_pid);
        child.transport.reset();
        child.example = nullptr;

        if (child.child_pid > 0)
        {
            int status = 0;
            ::waitpid(child.child_pid, &status, 0);
            RPC_INFO(
                "destroy_isolated_child: zone={} pid={} status={}", child.dll_zone.get_subnet(), child.child_pid, status);
            child.child_pid = -1;
        }

        if (child.queues)
        {
            ::munmap(child.queues, sizeof(rpc::libcoro_ipc_dynamic_dll::queue_pair));
            child.queues = nullptr;
        }

        if (!child.mapped_file.empty())
        {
            ::unlink(child.mapped_file.c_str());
            child.mapped_file.clear();
        }
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
