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
#include <transports/spsc/transport.h>
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <future>
#include <csignal>

int g_test_result = 0;

namespace comprehensive
{
    namespace v1
    {

#ifdef CANOPY_BUILD_COROUTINE
        struct spsc_queues
        {
            rpc::spsc::queue_type to_process_2;
            rpc::spsc::queue_type to_process_1;
        };

        CORO_TASK(void)
        process_1_task(std::shared_ptr<coro::io_scheduler> scheduler,
            rpc::zone zone_1,
            rpc::zone zone_2,
            spsc_queues* queues,
            rpc::shared_ptr<i_calculator> local_calculator,
            std::atomic<bool>& is_loaded)
        {
            auto service_1 = std::make_shared<rpc::service>("process_1", zone_1, scheduler);
            comprehensive_idl_register_stubs(service_1);

            auto transport_1 = rpc::spsc::spsc_transport::create(
                "transport_1", service_1, zone_2, &queues->to_process_1, &queues->to_process_2, nullptr);

            scheduler->spawn(transport_1->pump_send_and_receive());

            rpc::shared_ptr<comprehensive::v1::i_calculator> remote_calculator;
            std::cout << "Process 1: Connecting...\n";
            auto error
                = CO_AWAIT service_1->connect_to_zone("process_2", transport_1, local_calculator, remote_calculator);

            is_loaded = true;

            if (error == rpc::error::OK())
            {
                std::cout << "Process 1: Connected!\n";
                std::cout << "Process 1: testing...\n";

                int result;
                auto error = CO_AWAIT remote_calculator->add(10, 20, result);
                std::cout << "Process 2: add(10, 20) = " << result << " (error: " << static_cast<int>(error) << ")\n";
                g_test_result = (error == rpc::error::OK()) ? result : -error;

                error = CO_AWAIT remote_calculator->multiply(7, 8, result);
                std::cout << "Process 2: multiply(7, 8) = " << result << " (error: " << static_cast<int>(error) << ")\n";

                remote_calculator.reset();
                if (error == rpc::error::OK())
                {
                    g_test_result = result;
                }
            }
            else
            {
                std::cout << "Process 1: Connect failed: " << static_cast<int>(error) << "\n";
            }

            // int64_t count = transport_1->get_destination_count();
            // while (count)
            // {
            //     // RPC_DEBUG("1 -> {}", count);
            //     CO_AWAIT scheduler->schedule();
            //     count = transport_1->get_destination_count();
            // }
            CO_AWAIT transport_1->shutdown();
        }

        CORO_TASK(void)
        process_2_task(std::shared_ptr<coro::io_scheduler> scheduler,
            rpc::zone zone_2,
            rpc::zone zone_1,
            spsc_queues* queues,
            std::atomic<bool>& is_loaded)
        {
            auto service_2 = std::make_shared<rpc::service>("process_2", zone_2, scheduler);
            comprehensive_idl_register_stubs(service_2);

            auto handler = [&, zone_1](const rpc::interface_descriptor& input_interface,
                               rpc::interface_descriptor& output_interface,
                               std::shared_ptr<rpc::service> service,
                               std::shared_ptr<rpc::spsc::spsc_transport> transport) -> CORO_TASK(int)
            {
                auto ret = CO_AWAIT service->attach_remote_zone<i_calculator, i_calculator>("process_1_proxy",
                    transport,
                    input_interface,
                    output_interface,
                    [&](const rpc::shared_ptr<i_calculator>& parent,
                        rpc::shared_ptr<i_calculator>& new_service,
                        const std::shared_ptr<rpc::service>& service_ptr) -> CORO_TASK(int)
                    {
                        new_service = rpc::shared_ptr<i_calculator>(new calculator_impl(service_ptr));
                        CO_RETURN rpc::error::OK();
                    });
                CO_RETURN ret;
            };

            auto transport_2 = rpc::spsc::spsc_transport::create(
                "transport_2", service_2, zone_1, &queues->to_process_2, &queues->to_process_1, handler);

            service_2->add_transport(zone_1.as_destination(), transport_2);
            scheduler->spawn(transport_2->pump_send_and_receive());

            while (transport_2->get_status() != rpc::transport_status::DISCONNECTED)
            {
                // RPC_DEBUG("2 -> {}", count);
                CO_AWAIT scheduler->schedule();
            }
        }

        void run_spsc_demo()
        {
            std::cout << "=== SPSC Transport Demo ===\n";

            rpc::zone zone_1{1};
            rpc::zone zone_2{2};
            auto queues = std::make_shared<spsc_queues>();

            auto scheduler_1 = coro::io_scheduler::make_shared(
                coro::io_scheduler::options{.thread_strategy = coro::io_scheduler::thread_strategy_t::spawn,
                    .pool = coro::thread_pool::options{.thread_count = 1},
                    .execution_strategy = coro::io_scheduler::execution_strategy_t::process_tasks_on_thread_pool});

            auto scheduler_2 = coro::io_scheduler::make_shared(
                coro::io_scheduler::options{.thread_strategy = coro::io_scheduler::thread_strategy_t::spawn,
                    .pool = coro::thread_pool::options{.thread_count = 1},
                    .execution_strategy = coro::io_scheduler::execution_strategy_t::process_tasks_on_thread_pool});

            auto local_calculator = rpc::shared_ptr<i_calculator>(new calculator_impl());
            std::atomic<bool> is_loaded = false;

            coro::sync_wait(
                coro::when_all(process_1_task(scheduler_1, zone_1, zone_2, queues.get(), local_calculator, is_loaded),
                    process_2_task(scheduler_2, zone_2, zone_1, queues.get(), is_loaded)));
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
            std::cout << "[DEBUG] " << message << std::endl;
            break;
        case 1:
            std::cout << "[TRACE] " << message << std::endl;
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
    for (int i = 0; i < 100; ++i)
    {
        comprehensive::v1::run_spsc_demo();
    }
    return (g_test_result == 30) ? 0 : 1;
#endif
}
