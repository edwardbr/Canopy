/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

/*
 *   Local Transport Demo
 *   Demonstrates in-process communication using local transport
 *
 *   Concept: Client and server in the same process using local transport
 *   - Both ends are in the same process and zone
 *   - Communication is direct function calls through stubs/proxies
 *   - No network or IPC overhead
 *
 *   To build and run:
 *   1. cmake --preset Debug (or Coroutine_Debug)
 *   2. cmake --build build --target local_transport_demo
 *   3. ./build/output/debug/demos/comprehensive/local_transport_demo
 */

#include <transports/local/transport.h>
#include <demo_impl.h>
#include <rpc/rpc.h>
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <fmt/core.h>

namespace comprehensive
{
    namespace v1
    {

        void print_separator(const std::string& title)
        {
            std::cout << "\n";
            std::cout << std::string(60, '=') << "\n";
            std::cout << "  " << title << "\n";
            std::cout << std::string(60, '=') << "\n";
        }

        CORO_TASK(bool)
        run_local_transport_demo(
#ifdef CANOPY_BUILD_COROUTINE
            std::shared_ptr<coro::io_scheduler> scheduler
#endif
        )
        {
            print_separator("LOCAL TRANSPORT DEMO (Coroutine Mode)");

            std::atomic<uint64_t> zone_gen{0};

            // Create root service in Zone 1
            auto root_service = std::make_shared<rpc::service>("root_service",
                rpc::zone{++zone_gen}
#ifdef CANOPY_BUILD_COROUTINE
                ,
                scheduler
#endif
            );

            std::cout << "Created root service in Zone " << root_service->get_zone_id().get_val() << "\n";

            // Create calculator in same zone (direct access)
            auto calculator = create_calculator();
            std::cout << "Created calculator in same zone\n";

            // Create data processor
            auto data_processor = create_data_processor();
            std::cout << "Created data processor\n";

            // Create child zone (Zone 2)
            rpc::zone child_zone_id{++zone_gen};
            std::cout << "Creating child service in Zone " << child_zone_id.get_val() << "\n";

            // Create child transport connecting to parent
            auto child_transport
                = std::make_shared<rpc::local::child_transport>("child_service", root_service, child_zone_id);

            child_transport->set_child_entry_point<i_demo_service, i_demo_service>(
                [&](const rpc::shared_ptr<i_demo_service>& parent,
                    rpc::shared_ptr<i_demo_service>& new_service,
                    const std::shared_ptr<rpc::child_service>& child_service_ptr) -> CORO_TASK(int)
                {
                    new_service = create_demo_service("child_service", child_service_ptr);
                    CO_RETURN rpc::error::OK();
                });

            // Connect parent to child
            rpc::shared_ptr<i_demo_service> child_service;
            rpc::shared_ptr<i_demo_service> input_service(new demo_service_impl("input", root_service));
            auto error
                = CO_AWAIT root_service->connect_to_zone("child_service", child_transport, input_service, child_service);

            if (error != rpc::error::OK())
            {
                std::cout << "Failed to connect to child zone: " << static_cast<int>(error) << "\n";
                CO_RETURN false;
            }

            std::cout << "Connected Zone " << root_service->get_zone_id().get_val() << " -> Zone "
                      << child_zone_id.get_val() << "\n";

            std::cout << "--- Making RPC calls through local transport ---\n";

            // Call calculator in same zone (direct)
            int result;
            error = CO_AWAIT calculator->add(10, 20, result);
            std::cout << "Local call: 10 + 20 = " << result << " (error: " << static_cast<int>(error) << ")\n";

            error = CO_AWAIT calculator->multiply(7, 8, result);
            std::cout << "Local call: 7 * 8 = " << result << " (error: " << static_cast<int>(error) << ")\n";

            // Call through child service
            uint64_t child_zone_id_result;
            error = CO_AWAIT child_service->get_zone_id(child_zone_id_result);
            std::cout << "Remote call: Child zone ID = " << child_zone_id_result
                      << " (error: " << static_cast<int>(error) << ")\n";

            std::string service_name;
            error = CO_AWAIT child_service->get_name(service_name);
            std::cout << "Remote call: Child service name = " << service_name << " (error: " << static_cast<int>(error)
                      << ")\n";

            // Test data processor
            std::vector<int> input{1, 2, 3, 4, 5};
            std::vector<int> output;
            error = CO_AWAIT data_processor->process_vector(input, output);
            std::string output_str;
            for (size_t i = 0; i < output.size(); ++i)
            {
                output_str += std::to_string(output[i]);
                if (i < output.size() - 1)
                    output_str += ",";
            }
            std::cout << "Data processor: process_vector {1,2,3,4,5} -> {" << output_str
                      << "} (error: " << static_cast<int>(error) << ")\n";

            // Test child service
            std::string child_response;
            error = CO_AWAIT child_service->echo_through_child("Hello from parent", child_response);
            std::cout << "Child echo: " << child_response << " (error: " << static_cast<int>(error) << ")\n";

            std::cout << "========================================\n";
            std::cout << "  LOCAL TRANSPORT DEMO COMPLETED\n";
            std::cout << "========================================\n";
            CO_RETURN true;
        }

        CORO_TASK(void)
        demo_task(
#ifdef CANOPY_BUILD_COROUTINE
            std::shared_ptr<coro::io_scheduler> scheduler,
#endif
            bool* result_flag,
            bool* completed_flag)
        {
            bool res = CO_AWAIT run_local_transport_demo(
#ifdef CANOPY_BUILD_COROUTINE
                scheduler
#endif
            );
            *result_flag = res;
            *completed_flag = true;
            CO_RETURN;
        }
    }
}

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

int main()
{
    std::cout << "RPC++ Comprehensive Demo - Local Transport\n";
    std::cout << "========================================\n";
    std::cout << "Demonstrates in-process communication using local transport\n";
    std::cout << "\n";

#ifdef CANOPY_BUILD_COROUTINE
    auto scheduler = coro::io_scheduler::make_shared(
        coro::io_scheduler::options{
            .thread_strategy = coro::io_scheduler::thread_strategy_t::spawn,
            .pool = coro::thread_pool::options{
                .thread_count = std::thread::hardware_concurrency(),
            },
            .execution_strategy = coro::io_scheduler::execution_strategy_t::process_tasks_on_thread_pool
        });

    bool result = false;
    bool completed = false;

    scheduler->spawn(comprehensive::v1::demo_task(scheduler, &result, &completed));

    while (!completed)
    {
        scheduler->process_events(std::chrono::milliseconds(1));
    }

    return result ? 0 : 1;
#else
    return comprehensive::v1::run_local_transport_demo() ? 0 : 1;
#endif
}
