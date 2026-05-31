/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#ifdef CANOPY_BUILD_COROUTINE

#  include <atomic>
#  include <chrono>
#  include <memory>
#  include <string>
#  include <thread>

#  include <common/tests.h>
#  include <test_host.h>
#  include <transports/ipc_spsc/transport.h>

namespace
{
    struct command_line
    {
        std::string mode;
        std::string shared_memory_file;
    };

    const char* option_value(
        int argc,
        char** argv,
        const char* name)
    {
        const std::string expected = std::string("--") + name;
        for (int index = 1; index + 1 < argc; ++index)
        {
            if (argv[index] && expected == argv[index])
                return argv[index + 1];
        }
        return nullptr;
    }

    command_line parse_command_line(
        int argc,
        char** argv)
    {
        command_line result;
        if (auto* mode = option_value(argc, argv, "mode"))
            result.mode = mode;
        if (auto* shared_memory_file = option_value(argc, argv, "shared-memory-file"))
            result.shared_memory_file = shared_memory_file;
        return result;
    }

    rpc::zone make_zone(uint64_t subnet_offset)
    {
        auto address = rpc::DEFAULT_PREFIX;
        [[maybe_unused]] auto ok = address.set_subnet(address.get_subnet() + subnet_offset);
        RPC_ASSERT(ok);
        return rpc::zone(address);
    }

    std::shared_ptr<coro::scheduler> make_scheduler()
    {
        return std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
            coro::scheduler::options{
                .thread_strategy = coro::scheduler::thread_strategy_t::manual,
                .pool = coro::thread_pool::options{.thread_count = 1},
            }));
    }

    bool pump_until(
        const std::shared_ptr<coro::scheduler>& scheduler,
        const std::atomic_bool& done,
        std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!done.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
            scheduler->process_events(std::chrono::milliseconds{1});
        return done.load(std::memory_order_acquire);
    }

    CORO_TASK(void)
    run_acceptor(
        std::shared_ptr<rpc::root_service> service,
        std::string shared_memory_file,
        int& exit_code,
        std::atomic_bool& done)
    {
        auto acceptor = CO_AWAIT rpc::ipc_spsc::make_peer_acceptor<yyy::i_host, yyy::i_example>(
            "ipc_spsc_peer_acceptor",
            service,
            rpc::ipc_spsc::shared_memory_file_options{
                .path = std::move(shared_memory_file), .create = true, .unlink_on_destroy = true},
            [](rpc::shared_ptr<yyy::i_host> host,
                std::shared_ptr<rpc::service> svc) -> CORO_TASK(rpc::service_connect_result<yyy::i_example>)
            {
                CO_RETURN rpc::service_connect_result<yyy::i_example>{
                    rpc::error::OK(), rpc::shared_ptr<yyy::i_example>(new marshalled_tests::example(svc, host))};
            });
        if (!acceptor)
        {
            exit_code = 2;
            done.store(true, std::memory_order_release);
            CO_RETURN;
        }

        auto accept_error = CO_AWAIT acceptor->accept();
        if (accept_error != rpc::error::OK())
        {
            exit_code = 3;
            done.store(true, std::memory_order_release);
            CO_RETURN;
        }

        while (acceptor->get_status() != rpc::transport_status::DISCONNECTED)
            CO_AWAIT service->get_scheduler()->schedule_after(std::chrono::milliseconds{1});

        exit_code = 0;
        done.store(true, std::memory_order_release);
        CO_RETURN;
    }

    CORO_TASK(void)
    run_connector(
        std::shared_ptr<rpc::root_service> service,
        std::string shared_memory_file,
        int& exit_code,
        std::atomic_bool& done)
    {
        auto transport = rpc::ipc_spsc::make_peer_client(
            "ipc_spsc_peer_connector",
            service,
            rpc::ipc_spsc::shared_memory_file_options{.path = std::move(shared_memory_file)});
        if (!transport)
        {
            exit_code = 4;
            done.store(true, std::memory_order_release);
            CO_RETURN;
        }

        rpc::shared_ptr<yyy::i_host> local_host(new host());
        auto connect_result
            = CO_AWAIT service->connect_to_zone<yyy::i_host, yyy::i_example>("ipc_spsc_peer", transport, local_host);
        if (connect_result.error_code != rpc::error::OK() || !connect_result.output_interface)
        {
            exit_code = 5;
            done.store(true, std::memory_order_release);
            CO_RETURN;
        }

        int sum = 0;
        auto add_error = CO_AWAIT connect_result.output_interface->add(19, 23, sum);
        if (add_error != rpc::error::OK() || sum != 42)
        {
            exit_code = 6;
            done.store(true, std::memory_order_release);
            CO_RETURN;
        }

        connect_result.output_interface = nullptr;
        local_host = nullptr;
        while (transport->get_status() != rpc::transport_status::DISCONNECTED)
            CO_AWAIT service->get_scheduler()->schedule_after(std::chrono::milliseconds{1});

        exit_code = 0;
        done.store(true, std::memory_order_release);
        CO_RETURN;
    }
}

int main(
    int argc,
    char** argv)
{
    auto command = parse_command_line(argc, argv);
    if (command.shared_memory_file.empty())
        return 1;

    auto scheduler = make_scheduler();
    std::atomic_bool done{false};
    int exit_code = 1;

    if (command.mode == "acceptor")
    {
        auto service = rpc::root_service::create("ipc_spsc_peer_acceptor", make_zone(1), scheduler);
        current_host_service = service;
        if (!scheduler->spawn_detached(run_acceptor(service, command.shared_memory_file, exit_code, done)))
            return 7;
    }
    else if (command.mode == "connector")
    {
        auto service = rpc::root_service::create("ipc_spsc_peer_connector", rpc::DEFAULT_PREFIX, scheduler);
        current_host_service = service;
        if (!scheduler->spawn_detached(run_connector(service, command.shared_memory_file, exit_code, done)))
            return 8;
    }
    else
    {
        return 9;
    }

    if (!pump_until(scheduler, done, std::chrono::seconds{30}))
        return 10;

    scheduler->shutdown();
    current_host_service.reset();
    return exit_code;
}

#else

int main()
{
    return 1;
}

#endif
