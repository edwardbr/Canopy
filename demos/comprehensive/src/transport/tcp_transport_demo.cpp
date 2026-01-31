/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

/*
 *   TCP Transport Demo
 *   Demonstrates network communication using TCP transport
 *
 *   Concept: Client and server communicating over TCP/IP
 *   - Server: Listens on a port, accepts connections
 *   - Client: Connects to server, makes RPC calls
 *   - Requires: CANOPY_BUILD_COROUTINE=ON (uses async I/O)
 *
 *   MISCONCEPTION REPORT:
 *   ---------------------
 *   This demo requires CANOPY_BUILD_COROUTINE=ON because TCP transport uses
 *   libcoro for async I/O operations. The coro::net::tcp::client and
 *   coro::net::tcp::server classes are only available with coroutines.
 *
 *   Without coroutines, you would need to implement a synchronous TCP
 *   transport wrapper, which is not provided in the base RPC++ library.
 *
 *   To build and run:
 *   1. cmake --preset Coroutine_Debug
 *   2. cmake --build build --target tcp_transport_demo
 *   3. ./build/output/debug/demos/comprehensive/tcp_transport_demo
 */

#include <demo_impl.h>
#include <rpc/rpc.h>
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <csignal>

#include <transports/tcp/transport.h>
#include <transports/tcp/listener.h>
#include <comprehensive/comprehensive_stub.h>

void print_separator(const std::string& title)
{
    std::cout << "\n";
    std::cout << std::string(60, '=') << "\n";
    std::cout << "  " << title << "\n";
    std::cout << std::string(60, '=') << "\n";
}

namespace comprehensive
{
    namespace v1
    {

#ifdef CANOPY_BUILD_COROUTINE
        CORO_TASK(bool)
        run_tcp_server(
            std::shared_ptr<coro::io_scheduler> scheduler, rpc::event& server_ready, const rpc::event& client_finished)
        {
            print_separator("TCP SERVER (Coroutine Mode)");

            std::atomic<uint64_t> zone_gen{0};
            const uint16_t port = 18888;
            auto on_shutdown_event = std::make_shared<rpc::event>();

            // Create server service
            auto service = std::make_shared<rpc::service>("tcp_server", rpc::zone{++zone_gen}, scheduler);
            service->set_shutdown_event(on_shutdown_event);
            comprehensive_idl_register_stubs(service);

            std::cout << "Server zone ID: " << service->get_zone_id().get_val() << "\n";

            // Create TCP listener with connection handler
            auto listener = std::make_shared<rpc::tcp::listener>(
                [](const rpc::interface_descriptor& input_descr,
                    rpc::interface_descriptor& output_interface,
                    std::shared_ptr<rpc::service> child_service_ptr,
                    std::shared_ptr<rpc::tcp::tcp_transport> transport) -> CORO_TASK(int)
                {
                    std::cout << "Server: Accepting connection from zone " << input_descr.destination_zone_id.get_val()
                              << "\n";

                    // Use attach_remote_zone to handle the connection
                    auto ret
                        = CO_AWAIT child_service_ptr->attach_remote_zone<i_calculator, i_calculator>("tcp_client_proxy",
                            transport,
                            input_descr,
                            output_interface,
                            [](const rpc::shared_ptr<i_calculator>& remote_calc,
                                rpc::shared_ptr<i_calculator>& local_calc,
                                const std::shared_ptr<rpc::service>& service_ptr) -> CORO_TASK(int)
                            {
                                // Create local calculator implementation
                                local_calc = rpc::shared_ptr<i_calculator>(new calculator_impl(service_ptr));
                                std::cout << "Server: Created calculator service\n";
                                CO_RETURN rpc::error::OK();
                            });

                    if (ret == rpc::error::OK())
                    {
                        std::cout << "Server: Client connected successfully\n";
                    }
                    else
                    {
                        std::cout << "Server: Client connection failed: " << static_cast<int>(ret) << "\n";
                    }

                    CO_RETURN ret;
                },
                std::chrono::milliseconds(100000));

            // Start listening
            auto server_options = coro::net::tcp::server::options{
                .address = coro::net::ip_address::from_string("127.0.0.1"), .port = port, .backlog = 10};

            if (!listener->start_listening(service, server_options))
            {
                std::cout << "Server: Failed to start listening\n";
                CO_RETURN false;
            }

            std::cout << "Server: Listening on port " << port << "\n";

            // the service is maintained by the listener and transport
            service.reset();

            server_ready.set();

            // wait for the client to finish
            co_await client_finished.wait();

            // the listener is no longer needed and it has a shared pointer to the service, so it needs to go
            // now the lifetime of the service is maintained by the reference counts to it maintained by the stubs,
            // proxies and passthroughs that are using it
            co_await listener->stop_listening();
            listener.reset();

            co_await on_shutdown_event->wait();

            print_separator("TCP SERVER SHUTDOWN");
            CO_RETURN true;
        }

        CORO_TASK(bool)
        run_tcp_client(
            std::shared_ptr<coro::io_scheduler> scheduler, const rpc::event& server_ready, rpc::event& client_finished)
        {
            print_separator("TCP CLIENT (Coroutine Mode)");
            {
                // Wait for server to be ready
                co_await server_ready.wait();

                std::atomic<uint64_t> zone_gen{100}; // Use different zone IDs for client
                const uint16_t port = 18888;
                const char* host = "127.0.0.1";

                // Server zone ID (must match server's zone)
                auto peer_zone_id = rpc::zone{1};

                // Create client service
                auto client_service = std::make_shared<rpc::service>("tcp_client", rpc::zone{++zone_gen}, scheduler);
                comprehensive_idl_register_stubs(client_service);

                std::cout << "Client zone ID: " << client_service->get_zone_id().get_val() << "\n";
                std::cout << "Client: Connecting to " << host << ":" << port << "...\n";

                // Create TCP client
                coro::net::tcp::client client(scheduler,
                    coro::net::tcp::client::options{.address = coro::net::ip_address::from_string(host), .port = port});

                // Connect to server
                auto connection_status = CO_AWAIT client.connect(std::chrono::milliseconds(5000));
                if (connection_status != coro::net::connect_status::connected)
                {
                    std::cout << "Client: Failed to connect to server (status: " << static_cast<int>(connection_status)
                              << ")\n";
                    CO_RETURN false;
                }

                std::cout << "Client: TCP connection established\n";

                // Create TCP transport
                auto client_transport = rpc::tcp::tcp_transport::create("client_transport",
                    client_service,
                    peer_zone_id,
                    std::chrono::milliseconds(100000),
                    std::move(client),
                    nullptr); // Client doesn't need connection handler

                // Start the pump - must be done before connect_to_zone
                client_service->spawn(client_transport->pump_send_and_receive());

                std::cout << "Client: Starting RPC connection...\n";

                rpc::shared_ptr<i_calculator> local_calculator;

                // Remote calculator proxy (will be filled by connect_to_zone)
                rpc::shared_ptr<i_calculator> remote_calculator;

                // Connect to remote zone
                auto error = CO_AWAIT client_service->connect_to_zone(
                    "tcp_server", client_transport, local_calculator, remote_calculator);

                if (error != rpc::error::OK())
                {
                    std::cout << "Client: Failed to connect to zone: " << static_cast<int>(error) << "\n";
                    CO_RETURN false;
                }

                std::cout << "Client: RPC connection established\n";

                client_finished.set();

                std::cout << "Client: Making remote RPC calls...\n";

                // Make actual RPC calls over TCP
                int result;
                error = CO_AWAIT remote_calculator->add(100, 200, result);
                std::cout << "Client: add(100, 200) = " << result << " (error: " << static_cast<int>(error) << ")\n";

                error = CO_AWAIT remote_calculator->multiply(7, 8, result);
                std::cout << "Client: multiply(7, 8) = " << result << " (error: " << static_cast<int>(error) << ")\n";

                error = CO_AWAIT remote_calculator->subtract(500, 200, result);
                std::cout << "Client: subtract(500, 200) = " << result << " (error: " << static_cast<int>(error) << ")\n";

                // Clean up - this should trigger automatic transport shutdown
                remote_calculator.reset();
                std::cout << "Client: Released remote calculator\n";
            }
            print_separator("TCP CLIENT SHUTDOWN");
            CO_RETURN true;
        }
#endif
    }
}

void rpc_log(int level, const char* str, size_t sz)
{
    std::string message(str, sz);
    switch (level)
    {
    case 0:
        printf("[DEBUG] %s\n", message.c_str());
        break;
    case 1:
        printf("[TRACE] %s\n", message.c_str());
        break;
    case 2:
        printf("[INFO] %s\n", message.c_str());
        break;
    case 3:
        printf("[WARN] %s\n", message.c_str());
        break;
    case 4:
        printf("[ERROR] %s\n", message.c_str());
        break;
    case 5:
        printf("[CRITICAL] %s\n", message.c_str());
        break;
    default:
        printf("[LOG %d] %s\n", level, message.c_str());
        break;
    }
}

int main()
{
    std::cout << "RPC++ Comprehensive Demo - TCP Transport\n";
    std::cout << "========================================\n";
    std::cout << "NOTE: TCP transport demo requires CANOPY_BUILD_COROUTINE=ON\n";
    std::cout << "\n";

#ifndef CANOPY_BUILD_COROUTINE
    std::cout << "TCP transport requires coroutines.\n";
    std::cout << "Please configure with: cmake --preset Coroutine_Debug\n";
    return 1;
#else

    auto scheduler = coro::io_scheduler::make_shared(
        coro::io_scheduler::options{
            .thread_strategy = coro::io_scheduler::thread_strategy_t::spawn,
            .pool = coro::thread_pool::options{
                .thread_count = std::thread::hardware_concurrency(),
            },
            .execution_strategy = coro::io_scheduler::execution_strategy_t::process_tasks_on_thread_pool
        });

    for (int i = 0; i < 100; i++)
    {
        std::cout << "\n=== Test iteration " << (i + 1) << " ===\n";

        // this tells the client it can begin its tests
        rpc::event server_ready;
        // this tells that the client has finished so the server can shutdown
        rpc::event client_finished;

        coro::sync_wait(coro::when_all(comprehensive::v1::run_tcp_server(scheduler, server_ready, client_finished),
            comprehensive::v1::run_tcp_client(scheduler, server_ready, client_finished)));
    }

    print_separator("TCP TRANSPORT DEMO COMPLETED");
    return 0;
#endif
}
