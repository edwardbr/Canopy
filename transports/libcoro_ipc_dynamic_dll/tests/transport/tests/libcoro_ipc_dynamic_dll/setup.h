/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#ifdef CANOPY_BUILD_COROUTINE

#  include "test_host.h"
#  include "test_globals.h"

#  include <gtest/gtest.h>

#  include <common/tests.h>
#  include <common/transport_setup_base.h>
#  include <cstdio>
#  include <fcntl.h>
#  include <new>
#  include <spawn.h>
#  include <thread>
#  include <vector>
#  include <streaming/spsc_queue/stream.h>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <sys/wait.h>
#  include <transports/libcoro_ipc_dynamic_dll/transport.h>

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
    rpc::zone host_zone_ = rpc::DEFAULT_PREFIX;
    rpc::zone dll_zone_ = []
    {
        auto address = rpc::DEFAULT_PREFIX;
        [[maybe_unused]] bool ok = address.set_subnet(address.get_subnet() + 1);
        RPC_ASSERT(ok);
        return rpc::zone(address);
    }();

    std::shared_ptr<rpc::stream_transport::transport> client_transport_;
    bool startup_complete_ = false;

    CORO_TASK(bool) finish_setup(rpc::libcoro_ipc_dynamic_dll::queue_pair* queues)
    {
        this->root_service_ = std::make_shared<rpc::root_service>("host", host_zone_, this->io_scheduler_);
        current_host_service = this->root_service_;

        rpc::shared_ptr<yyy::i_host> hst(new host());
        this->local_host_ptr_ = hst;

        client_transport_ = rpc::libcoro_ipc_dynamic_dll::make_client("ipc_dll_client", this->root_service_, queues);

        auto connect_result = CO_AWAIT this->root_service_->template connect_to_zone<yyy::i_host, yyy::i_example>(
            "ipc child", client_transport_, hst);
        this->i_example_ptr_ = std::move(connect_result.output_interface);
        startup_complete_ = true;
        CO_RETURN connect_result.error_code == rpc::error::OK();
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
        while (!startup_complete_)
            this->io_scheduler_->process_events(std::chrono::milliseconds(1));
    }

    void common_teardown()
    {
        this->i_host_ptr_ = nullptr;
        this->i_example_ptr_ = nullptr;
        this->client_transport_.reset();
        this->root_service_ = nullptr;
        current_host_service.reset();
        this->reset_telemetry_for_test();
    }
};

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

        loaded_ = rpc::libcoro_ipc_dynamic_dll::loaded_library::load(CANOPY_TEST_LIBCORO_IPC_DLL_PATH,
            "libcoro_ipc_dynamic_dll",
            this->dll_zone_,
            this->host_zone_,
            &queues_.dll_to_host,
            &queues_.host_to_dll);
        ASSERT_NE(loaded_, nullptr);

        RPC_ASSERT(this->io_scheduler_->spawn_detached(this->check_for_error(this->finish_setup(&queues_))));
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

template<bool UseHostInChild, bool RunStandardTests, bool CreateNewZoneThenCreateSubordinatedZone>
class libcoro_ipc_dll_isolated_transport_setup
    : public libcoro_ipc_dll_setup_base<UseHostInChild, RunStandardTests, CreateNewZoneThenCreateSubordinatedZone>
{
    std::string mapped_file_;
    rpc::libcoro_ipc_dynamic_dll::queue_pair* queues_ = nullptr;
    pid_t child_pid_ = -1;

    void create_mapped_file()
    {
        char file_template[] = "/tmp/canopy_ipc_dll_XXXXXX";
        int fd = ::mkstemp(file_template);
        ASSERT_GE(fd, 0);
        mapped_file_ = file_template;
        ASSERT_EQ(::ftruncate(fd, sizeof(rpc::libcoro_ipc_dynamic_dll::queue_pair)), 0);
        queues_ = static_cast<rpc::libcoro_ipc_dynamic_dll::queue_pair*>(
            ::mmap(nullptr, sizeof(rpc::libcoro_ipc_dynamic_dll::queue_pair), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
        ::close(fd);
        ASSERT_NE(queues_, MAP_FAILED);
        new (queues_) rpc::libcoro_ipc_dynamic_dll::queue_pair{};
    }

    void spawn_loader()
    {
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(CANOPY_TEST_LIBCORO_IPC_LOADER_PATH));
        argv.push_back(const_cast<char*>(CANOPY_TEST_LIBCORO_IPC_DLL_PATH));
        argv.push_back(mapped_file_.data());
        argv.push_back(nullptr);
        ASSERT_EQ(
            ::posix_spawn(&child_pid_, CANOPY_TEST_LIBCORO_IPC_LOADER_PATH, nullptr, nullptr, argv.data(), environ), 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

public:
    void set_up()
    {
        this->start_telemetry_test();
        this->set_up_scheduler();
        create_mapped_file();
        spawn_loader();

        RPC_ASSERT(this->io_scheduler_->spawn_detached(this->check_for_error(this->finish_setup(queues_))));
        this->pump_until_startup();
        ASSERT_EQ(this->error_has_occurred_, false);
    }

    void tear_down()
    {
        this->common_teardown();

        if (child_pid_ > 0)
        {
            int status = 0;
            ::waitpid(child_pid_, &status, 0);
            child_pid_ = -1;
        }

        if (queues_)
        {
            ::munmap(queues_, sizeof(rpc::libcoro_ipc_dynamic_dll::queue_pair));
            queues_ = nullptr;
        }

        if (!mapped_file_.empty())
        {
            ::unlink(mapped_file_.c_str());
            mapped_file_.clear();
        }
    }
};

#endif // CANOPY_BUILD_COROUTINE
