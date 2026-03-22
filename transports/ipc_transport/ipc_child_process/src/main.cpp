/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#ifdef CANOPY_BUILD_COROUTINE

#  include <chrono>

#  include <common/foo_impl.h>
#  include <example/example.h>
#  include <streaming/spsc_queue/stream.h>
#  include <transports/ipc_transport/bootstrap.h>
#  include <transports/streaming/transport.h>

namespace
{
    CORO_TASK(rpc::service_connect_result<yyy::i_example>)
    make_example(rpc::shared_ptr<yyy::i_host> host, std::shared_ptr<rpc::service> svc)
    {
        auto example = rpc::shared_ptr<yyy::i_example>(new marshalled_tests::example(svc, host));
        CO_RETURN rpc::service_connect_result<yyy::i_example>{rpc::error::OK(), std::move(example)};
    }
}

int main(int argc, char** argv)
{
    auto bootstrap = rpc::ipc_transport::child_process_bootstrap::from_command_line(argc, argv);
    if (!bootstrap)
        return 1;

    auto* queues = bootstrap->map_queue_pair();
    if (!queues)
        return 2;

    auto scheduler = std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
        coro::scheduler::options{.thread_strategy = coro::scheduler::thread_strategy_t::manual,
            .pool = coro::thread_pool::options{.thread_count = 1}}));
    auto service = std::make_shared<rpc::root_service>("ipc_child_process", bootstrap->child_zone(), scheduler);

    auto stream = std::make_shared<streaming::spsc_queue::stream>(&queues->dll_to_host, &queues->host_to_dll, scheduler);
    auto acceptor = rpc::stream_transport::make_server<yyy::i_host, yyy::i_example>(
        "ipc_child_process", service, std::move(stream), &make_example);
    if (!acceptor)
        return 3;

    bool accept_complete = false;
    int accept_result = rpc::error::OK();
    RPC_ASSERT(scheduler->spawn_detached(
        [&]() -> coro::task<void>
        {
            accept_result = CO_AWAIT acceptor->accept();
            accept_complete = true;
            CO_RETURN;
        }()));

    while (!accept_complete)
        scheduler->process_events(std::chrono::milliseconds(1));

    if (accept_result != rpc::error::OK())
        return 4;

    while (acceptor->get_status() != rpc::transport_status::DISCONNECTED)
        scheduler->process_events(std::chrono::milliseconds(1));

    acceptor.reset();
    service.reset();
    scheduler->shutdown();

    rpc::ipc_transport::queue_pair_bootstrap::unmap_queue_pair(queues);
    return 0;
}

#endif // CANOPY_BUILD_COROUTINE
