/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#ifdef CANOPY_BUILD_COROUTINE

#  include "benchmark_data_processor.h"
#  include <streaming/spsc_queue/stream.h>
#  include <transports/ipc_transport/bootstrap.h>
#  include <transports/streaming/transport.h>

namespace
{
    CORO_TASK(rpc::service_connect_result<comprehensive::v1::i_data_processor>)
    make_data_processor(
        rpc::shared_ptr<comprehensive::v1::i_data_processor>,
        std::shared_ptr<rpc::service>)
    {
        CO_RETURN rpc::service_connect_result<comprehensive::v1::i_data_processor>{
            rpc::error::OK(), comprehensive::v1::make_benchmark_data_processor()};
    }

    CORO_TASK(int)
    run_child_process(
        std::shared_ptr<coro::scheduler> scheduler,
        rpc::zone child_zone,
        rpc::libcoro_spsc_dynamic_dll::queue_pair* queues)
    {
        auto service = rpc::root_service::create("benchmark_ipc_child_process", child_zone, scheduler);

        auto stream
            = std::make_shared<streaming::spsc_queue::stream>(&queues->dll_to_host, &queues->host_to_dll, scheduler);
        auto acceptor
            = rpc::stream_transport::make_server<comprehensive::v1::i_data_processor, comprehensive::v1::i_data_processor>(
                "benchmark_ipc_child_process", service, std::move(stream), &make_data_processor);
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

#endif
