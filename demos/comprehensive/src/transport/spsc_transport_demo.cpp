/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

/*
 *   SPSC Transport Demo
 *   Demonstrates Single-Producer Single-Consumer queue-based IPC
 *
 *   Architecture:
 *   - Two "processes" simulated by two threads with separate schedulers
 *   - Two SPSC queues in shared memory for bidirectional communication:
 *     * Queue 1: written by process 1, read by process 2
 *     * Queue 2: written by process 2, read by process 1
 *   - Each process has a service and transport connected to the other
 *   - Process 1 calls connect_to_zone
 *   - Process 2's transport handler calls attach_remote_zone
 */

#include <demo_impl.h>
#include <rpc/rpc.h>
#include <comprehensive/comprehensive_stub.h>
#include <streaming/spsc_queue/stream.h>
#include <transports/streaming/transport.h>
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <future>
#include <csignal>

namespace comprehensive
{
    namespace v1
    {

#ifdef CANOPY_BUILD_COROUTINE
        struct spsc_queues
        {
            streaming::spsc_queue::queue_type to_process_2;
            streaming::spsc_queue::queue_type to_process_1;
        };

        CORO_TASK(bool)
        process_1_task(bool& success,
            std::shared_ptr<coro::scheduler> scheduler,
            rpc::zone zone_1,
            rpc::zone zone_2,
            spsc_queues* queues,
            std::atomic<bool>& is_loaded,
            rpc::event& client_finished)
        {
            rpc::shared_ptr<comprehensive::v1::i_calculator> remote_calculator;
            auto on_shutdown_event = std::make_shared<rpc::event>();
            int error = rpc::error::OK();

            {
                auto service_1 = std::make_shared<rpc::root_service>("process_1", zone_1, scheduler);

                service_1->set_shutdown_event(on_shutdown_event);

                auto stream_1 = std::make_shared<streaming::spsc_queue::stream>(
                    &queues->to_process_1, &queues->to_process_2, scheduler);
                auto transport_1 = rpc::stream_transport::make_client("transport_1", service_1, std::move(stream_1));

                rpc::shared_ptr<i_calculator> local_calculator; // = rpc::shared_ptr<i_calculator>(new calculator_impl());

                std::cout << "Process 1: Connecting...\n";
                auto connect_result = CO_AWAIT service_1->connect_to_zone<i_calculator, comprehensive::v1::i_calculator>(
                    "process_2", transport_1, local_calculator);
                remote_calculator = connect_result.output_interface;
                auto error = connect_result.error_code;

                is_loaded = true;

                service_1.reset();
                transport_1.reset();
            }

            if (error == rpc::error::OK())
            {
                std::cout << "Process 1: Connected!\n";
                std::cout << "Process 1: testing...\n";

                int result = 0;
                error = CO_AWAIT remote_calculator->add(10, 20, result);
                std::cout << "Process 1: add(10, 20) = " << result << " (error: " << static_cast<int>(error) << ")\n";
                if (error != rpc::error::OK())
                {
                    success = false;
                }

                error = CO_AWAIT remote_calculator->multiply(7, 8, result);
                std::cout << "Process 1: multiply(7, 8) = " << result << " (error: " << static_cast<int>(error) << ")\n";

                if (error != rpc::error::OK())
                {
                    success = false;
                }

                std::cout << "Process 1: Released remote calculator\n";
            }
            else
            {
                std::cout << "Process 1: Connect failed: " << static_cast<int>(error) << "\n";
            }

            // Clean up - release remote objects before service/transport
            remote_calculator.reset();

            std::cout << "Process 1: Setting client_finished event\n";
            client_finished.set();

            // Service and transport will be destroyed when scope exits
            // Framework automatically handles disconnect and cleanup via reference counting
            std::cout << "Process 1: Waiting for shutdown event\n";
            co_await on_shutdown_event->wait();
            std::cout << "Process 1: Shutdown complete\n";
            co_return success;
        }

        CORO_TASK(void)
        process_2_task(std::shared_ptr<coro::scheduler> scheduler,
            rpc::zone zone_2,
            rpc::zone zone_1,
            spsc_queues* queues,
            std::atomic<bool>& is_loaded,
            rpc::event& server_ready,
            const rpc::event& client_finished)
        {
            auto on_shutdown_event = std::make_shared<rpc::event>();
            auto service_2 = std::make_shared<rpc::root_service>("process_2", zone_2, scheduler);
            service_2->set_shutdown_event(on_shutdown_event);

            rpc::event on_connected;

            auto stream_2 = std::make_shared<streaming::spsc_queue::stream>(
                &queues->to_process_2, &queues->to_process_1, scheduler);
            auto transport_2 = CO_AWAIT service_2->make_acceptor<i_calculator, i_calculator>("transport_2",
                rpc::stream_transport::transport_factory(std::move(stream_2)),
                // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
                [&](const rpc::shared_ptr<i_calculator>&,
                    const std::shared_ptr<rpc::service>& svc) -> CORO_TASK(rpc::service_connect_result<i_calculator>)
                {
                    on_connected.set();
                    std::cout << "Process 2: Created calculator service\n";
                    CO_RETURN rpc::service_connect_result<i_calculator>{
                        rpc::error::OK(), rpc::shared_ptr<i_calculator>(new calculator_impl(svc))};
                });

            co_await transport_2->accept();

            std::cout << "Process 2: Ready for connections\n";
            server_ready.set();

            co_await on_connected.wait();

            // Release references to allow cleanup
            service_2.reset();

            std::cout << "Process 2: Service reset\n";
            transport_2.reset();
            std::cout << "Process 2: Transport reset\n";

            // Wait for client to finish
            std::cout << "Process 2: Waiting for client to finish\n";
            co_await client_finished.wait();
            std::cout << "Process 2: Client finished, waiting for transport shutdown\n";

            // Wait for service shutdown event
            std::cout << "Process 2: Waiting for shutdown event\n";
            co_await on_shutdown_event->wait();
            std::cout << "Process 2: Shutdown complete\n";
        }

        bool run_spsc_demo()
        {
            std::cout << "=== SPSC Transport Demo ===\n";

            rpc::zone_id_allocator zone_gen_{rpc::DEFAULT_PREFIX};
            rpc::zone_address addr1;
            rpc::zone_address addr2;
            zone_gen_.allocate_zone(addr1);
            zone_gen_.allocate_zone(addr2);
            rpc::zone zone_1(addr1);
            rpc::zone zone_2(addr2);
            auto queues = std::make_shared<spsc_queues>();

            auto scheduler_1 = std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
                coro::scheduler::options{.thread_strategy = coro::scheduler::thread_strategy_t::spawn,
                    .pool = coro::thread_pool::options{.thread_count = 1},
                    .execution_strategy = coro::scheduler::execution_strategy_t::process_tasks_on_thread_pool}));

            auto scheduler_2 = std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
                coro::scheduler::options{.thread_strategy = coro::scheduler::thread_strategy_t::spawn,
                    .pool = coro::thread_pool::options{.thread_count = 1},
                    .execution_strategy = coro::scheduler::execution_strategy_t::process_tasks_on_thread_pool}));

            std::atomic<bool> is_loaded = false;

            // Event-based synchronization
            rpc::event server_ready;
            rpc::event client_finished;

            bool success = true;
            coro::sync_wait(coro::when_all(
                process_1_task(success, scheduler_1, zone_1, zone_2, queues.get(), is_loaded, client_finished),
                process_2_task(scheduler_2, zone_2, zone_1, queues.get(), is_loaded, server_ready, client_finished)));

            // Explicitly shutdown schedulers to terminate and join their spawned threads
            // This prevents thread accumulation across multiple iterations
            scheduler_1->shutdown();
            scheduler_2->shutdown();
            return success;
        }
#endif
    }
}

extern "C"
{
    void rpc_log(int level, const char* str, size_t sz)
    {
        std::string message(str, sz);
        switch (level)
        {
        case 0:
            std::cout << "[TRACE] " << message << std::endl;
            break;
        case 1:
            std::cout << "[DEBUG] " << message << std::endl;
            break;
        case 2:
            std::cout << "[INFO] " << message << std::endl;
            break;
        case 3:
            std::cout << "[WARN] " << message << std::endl;
            break;
        case 4:
            std::cout << "[ERROR] " << message << std::endl;
            break;
        case 5:
            std::cout << "[CRITICAL] " << message << std::endl;
            break;
        default:
            std::cout << "[LOG " << level << "] " << message << std::endl;
            break;
        }
    }
}

int main()
{
    std::cout << "SPSC Transport Demo - Two Process Architecture\n";

#ifndef CANOPY_BUILD_COROUTINE
    std::cout << "Requires coroutines\n";
    return 1;
#else
    int test_result = 0;

    for (int i = 0; i < 100; ++i)
    {
        test_result += comprehensive::v1::run_spsc_demo() ? 1 : 0;
    }
    return (test_result == 30) ? 0 : 1;
#endif
}
