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
#include <connection_factory/options.h>
#include <streaming/spsc_queue/factory.h>
#include <comprehensive/comprehensive_stub.h>
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
        namespace calc = ::calculator::v1;

#ifdef CANOPY_BUILD_COROUTINE
        CORO_TASK(bool)
        process_1_task(
            bool& success,
            std::shared_ptr<coro::scheduler> scheduler,
            rpc::zone zone_1,
            rpc::zone zone_2,
            rpc::spsc_queue::queue_pair queues,
            std::atomic<bool>& is_loaded,
            rpc::event& client_finished)
        {
            (void)zone_2;

            rpc::shared_ptr<calc::i_calculator> remote_calculator;
            auto on_shutdown_event = std::make_shared<rpc::event>();
            int error = rpc::error::OK();

            {
                auto service_1 = rpc::root_service::create("process_1", zone_1, scheduler);

                service_1->set_shutdown_event(on_shutdown_event);

                rpc::shared_ptr<calc::i_calculator> local_calculator;

                rpc::connection_factory::stream_rpc_connection_settings options;
                options.service.name = "process_1";
                options.transport.name = "transport_1";
                options.transport.service_proxy_name = "process_2";
                options.transport.call_timeout_sweep = uint64_t{1};

                std::cout << "Process 1: Connecting...\n";
                auto connect_result = CO_AWAIT rpc::spsc_queue::connect_rpc<calc::i_calculator, calc::i_calculator>(
                    local_calculator, queues, options, service_1);
                remote_calculator = connect_result.output_interface;
                error = connect_result.error_code;

                is_loaded = true;

                service_1.reset();
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
        process_2_task(
            std::shared_ptr<coro::scheduler> scheduler,
            rpc::zone zone_2,
            rpc::zone zone_1,
            rpc::spsc_queue::queue_pair queues,
            std::atomic<bool>& is_loaded,
            rpc::event& server_ready,
            const rpc::event& client_finished)
        {
            (void)zone_1;
            (void)is_loaded;

            auto on_shutdown_event = std::make_shared<rpc::event>();
            auto service_2 = rpc::root_service::create("process_2", zone_2, scheduler);
            service_2->set_shutdown_event(on_shutdown_event);

            rpc::event on_connected;

            rpc::connection_factory::stream_rpc_connection_settings options;
            options.service.name = "process_2";
            options.transport.name = "transport_2";
            options.transport.service_proxy_name = "process_2";
            options.transport.call_timeout_sweep = uint64_t{1};

            auto accept_result = CO_AWAIT rpc::spsc_queue::accept_rpc<calc::i_calculator, calc::i_calculator>(
                // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
                [&](const rpc::shared_ptr<calc::i_calculator>&,
                    const std::shared_ptr<rpc::service>& svc) -> CORO_TASK(rpc::service_connect_result<calc::i_calculator>)
                {
                    on_connected.set();
                    std::cout << "Process 2: Created calculator service\n";
                    CO_RETURN rpc::service_connect_result<calc::i_calculator>{rpc::error::OK(), calc::make_calculator(svc)};
                },
                queues,
                options,
                service_2);
            if (accept_result.error_code != rpc::error::OK() || !accept_result.handle)
            {
                std::cout << "Process 2: Accept failed: " << static_cast<int>(accept_result.error_code) << "\n";
                server_ready.set();
                CO_RETURN;
            }
            auto connection = std::move(accept_result.handle);

            std::cout << "Process 2: Ready for connections\n";
            server_ready.set();

            co_await on_connected.wait();

            // Release references to allow cleanup
            service_2.reset();

            std::cout << "Process 2: Service reset\n";
            connection.reset();
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
            auto queues = rpc::spsc_queue::queue_pair::create();

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
            coro::sync_wait(
                coro::when_all(
                    process_1_task(success, scheduler_1, zone_1, zone_2, queues, is_loaded, client_finished),
                    process_2_task(scheduler_2, zone_2, zone_1, queues, is_loaded, server_ready, client_finished)));

            // Explicitly shutdown schedulers to terminate and join their spawned threads
            // This prevents thread accumulation across multiple iterations
            scheduler_1->shutdown();
            scheduler_2->shutdown();
            return success;
        }
#endif
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
