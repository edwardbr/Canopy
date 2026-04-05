/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#ifdef CANOPY_BUILD_COROUTINE

#  include <common/foo_impl.h>
#  include <example/example.h>
#  include <streaming/spsc_queue/stream.h>
#  include <transports/ipc_transport/bootstrap.h>
#  include <transports/streaming/transport.h>

namespace
{
    CORO_TASK(rpc::service_connect_result<yyy::i_example>)
    make_example(
        rpc::shared_ptr<yyy::i_host> host,
        std::shared_ptr<rpc::service> svc)
    {
        auto example = rpc::shared_ptr<yyy::i_example>(new marshalled_tests::example(svc, host));
        CO_RETURN rpc::service_connect_result<yyy::i_example>{rpc::error::OK(), std::move(example)};
    }

    CORO_TASK(int)
    run_child_process(
        std::shared_ptr<coro::scheduler> scheduler,
        rpc::zone child_zone,
        rpc::libcoro_spsc_dynamic_dll::queue_pair* queues)
    {
        auto service = std::make_shared<rpc::root_service>("ipc_child_process", child_zone, scheduler);

        auto stream
            = std::make_shared<streaming::spsc_queue::stream>(&queues->dll_to_host, &queues->host_to_dll, scheduler);
        auto acceptor = rpc::stream_transport::make_server<yyy::i_host, yyy::i_example>(
            "ipc_child_process", service, std::move(stream), &make_example);
        if (!acceptor)
            CO_RETURN 3;

        auto accept_result = CO_AWAIT acceptor->accept();
        if (accept_result != rpc::error::OK())
            CO_RETURN 4;

        while (acceptor->get_status() != rpc::transport_status::DISCONNECTED)
            CO_AWAIT scheduler->schedule();

        acceptor.reset();
        service.reset();
        CO_RETURN 0;
    }
}

int main(
    int argc,
    char** argv)
{
    auto bootstrap = rpc::ipc_transport::child_process_bootstrap::from_command_line(argc, argv);
    if (!bootstrap)
        return 1;

    auto* queues = bootstrap->map_queue_pair();
    if (!queues)
        return 2;

    auto scheduler = std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
        coro::scheduler::options{.thread_strategy = coro::scheduler::thread_strategy_t::spawn,
            .pool = coro::thread_pool::options{.thread_count = static_cast<uint32_t>(bootstrap->scheduler_thread_count())},
            .execution_strategy = coro::scheduler::execution_strategy_t::process_tasks_on_thread_pool}));

    int exit_code = 0;
    coro::sync_wait(
        coro::when_all(
            [&]() -> coro::task<void>
            {
                exit_code = CO_AWAIT run_child_process(scheduler, bootstrap->child_zone(), queues);
                CO_RETURN;
            }()));

    scheduler->shutdown();
    rpc::ipc_transport::queue_pair_bootstrap::unmap_queue_pair(queues);
    return exit_code;
}

#endif // CANOPY_BUILD_COROUTINE
