/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#if defined(CANOPY_BUILD_ENCLAVE) && defined(CANOPY_BUILD_COROUTINE)

#  include <atomic>
#  include <chrono>
#  include <memory>
#  include <string>

#  include <common/tests.h>
#  include <test_globals.h>
#  include <test_host.h>
#  include <transports/sgx_coroutine/host/connect.h>
#  include <transports/sgx_coroutine/host/transport.h>

namespace
{
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
    run_connector(
        std::shared_ptr<rpc::root_service> service,
        std::string shared_memory_file,
        int& exit_code,
        std::atomic_bool& done)
    {
        auto transport = std::make_shared<rpc::sgx_coroutine_transport::host::transport>(
            "sgx_coroutine_peer_connector", service, coroutine_enclave_path);
        transport->set_use_sidecar(true);
        transport->set_peer_to_peer_shared_memory_file(std::move(shared_memory_file));

        rpc::shared_ptr<yyy::i_host> local_host(new host());
        auto connect_result
            = CO_AWAIT rpc::sgx_coroutine_transport::host::connect_to_enclave_zone<yyy::i_host, yyy::i_example>(
                service, "sgx_coroutine_peer", transport, local_host);
        if (connect_result.error_code != rpc::error::OK() || !connect_result.output_interface)
        {
            exit_code = 2;
            done.store(true, std::memory_order_release);
            CO_RETURN;
        }

        int sum = 0;
        auto add_error = CO_AWAIT connect_result.output_interface->add(20, 22, sum);
        if (add_error != rpc::error::OK() || sum != 42)
        {
            exit_code = 3;
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
    auto* shared_memory_file = option_value(argc, argv, "shared-memory-file");
    if (!shared_memory_file)
        return 1;

    auto scheduler = make_scheduler();
    auto service = rpc::root_service::create("sgx_coroutine_peer_connector", rpc::DEFAULT_PREFIX, scheduler);
    current_host_service = service;

    std::atomic_bool done{false};
    int exit_code = 1;
    if (!scheduler->spawn_detached(run_connector(service, shared_memory_file, exit_code, done)))
        return 4;

    if (!pump_until(scheduler, done, std::chrono::seconds{40}))
        return 5;

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
