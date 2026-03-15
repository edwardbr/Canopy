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
 *   Build and run:
 *   1. cmake --preset Coroutine_Debug
 *   2. cmake --build build --target tcp_transport_demo
 *   3. ./build/output/debug/demos/comprehensive/tcp_transport_demo [options]
 *
 *   Options:
 *     --host <addr>         Server address to connect/listen on (default: 127.0.0.1)
 *     --port <n>            TCP port to connect/listen on (default: 18888)
 *     --routing-prefix <p>  Network routing prefix (auto-detected when omitted)
 *     -4 / -6               Interpret --routing-prefix as IPv4 / IPv6
 *     --subnet-offset <n>   Bit offset of subnet field in zone_address (default: 64)
 *     --object-offset <n>   Bit offset of object field in zone_address (default: 96)
 */

#include <demo_impl.h>
#include <rpc/rpc.h>
#include <iostream> // kept for args help/error output only
#include <chrono>

#include <streaming/listener.h>
#include <streaming/tcp/acceptor.h>
#include <streaming/tcp/stream.h>
#include <transports/streaming/transport.h>
#include <comprehensive/comprehensive_stub.h>

#include <canopy/network_config/network_args.h>

void print_separator(const std::string& title)
{
    RPC_INFO("\n{0}\n  {1}\n{0}", std::string(60, '='), title);
}

namespace comprehensive
{
    namespace v1
    {
        CORO_TASK(bool)
        run_tcp_server(std::shared_ptr<coro::scheduler> scheduler,
            rpc::event& server_ready,
            const rpc::event& client_finished,
            const canopy::network_config::network_config& cfg,
            rpc::zone server_zone)
        {
            std::string host = cfg.get_host_string();
            uint16_t port = cfg.port;

            print_separator("TCP SERVER (Coroutine Mode)");

            auto on_shutdown_event = std::make_shared<rpc::event>();

            // Create server service using the zone address provided by the allocator.
            auto service = std::make_shared<rpc::root_service>("tcp_server", server_zone, scheduler);
            service->set_shutdown_event(on_shutdown_event);

            RPC_INFO("Server zone ID (address): {}", rpc::to_yas_json<std::string>(service->get_zone_id().get_address()));

            const auto domain = cfg.host_family == canopy::network_config::ip_address_family::ipv6
                                    ? coro::net::domain_t::ipv6
                                    : coro::net::domain_t::ipv4;
            const coro::net::socket_address endpoint{coro::net::ip_address::from_string(host, domain), port};

            auto listener = std::make_shared<streaming::listener>("server_transport",
                std::make_shared<streaming::tcp::acceptor>(endpoint),
                rpc::stream_transport::make_connection_callback<i_calculator, i_calculator>(
                    [](const rpc::shared_ptr<i_calculator>&,
                        rpc::shared_ptr<i_calculator>& local,
                        const std::shared_ptr<rpc::service>& svc) -> CORO_TASK(int)
                    {
                        local = rpc::shared_ptr<i_calculator>(new calculator_impl(svc));
                        CO_RETURN rpc::error::OK();
                    }));

            if (!listener->start_listening(service))
            {
                RPC_ERROR("Server: Failed to start listening");
                CO_RETURN false;
            }

            RPC_INFO("Server: Listening on {}:{}", host, port);

            // The service is maintained by the listener and transport.
            service.reset();

            server_ready.set();

            // Wait for the client to finish.
            co_await client_finished.wait();

            // Stop the listener — the service lifetime is now maintained solely by the
            // reference counts held by stubs, proxies, and passthroughs inside it.
            co_await listener->stop_listening();
            listener.reset();

            co_await on_shutdown_event->wait();

            print_separator("TCP SERVER SHUTDOWN");
            CO_RETURN true;
        }

        CORO_TASK(bool)
        run_tcp_client(std::shared_ptr<coro::scheduler> scheduler,
            const rpc::event& server_ready,
            rpc::event& client_finished,
            const canopy::network_config::network_config& cfg,
            rpc::zone client_zone)
        {
            std::string host = cfg.get_host_string();
            uint16_t port = cfg.port;

            // Create client service using the zone address provided by the allocator.
            auto client_service = std::make_shared<rpc::root_service>("tcp_client", client_zone, scheduler);

            rpc::shared_ptr<i_calculator> remote_calculator;

            {
                // Wait for server to be ready before connecting.
                co_await server_ready.wait();

                RPC_INFO("Client zone ID (address): {}",
                    rpc::to_yas_json<std::string>(client_service->get_zone_id().get_address()));

                RPC_INFO("Client: Connecting to {}:{}...", host, port);
            }

            // connect to remote zone
            {
                // Create TCP client.
                const auto client_domain = cfg.host_family == canopy::network_config::ip_address_family::ipv6
                                               ? coro::net::domain_t::ipv6
                                               : coro::net::domain_t::ipv4;
                coro::net::tcp::client client(scheduler,
                    coro::net::socket_address{coro::net::ip_address::from_string(host, client_domain), port});

                auto connection_status = CO_AWAIT client.connect(std::chrono::milliseconds(5000));
                if (connection_status != coro::net::connect_status::connected)
                {
                    RPC_ERROR("Client: Failed to connect to server (status: {})", static_cast<int>(connection_status));
                    CO_RETURN false;
                }

                RPC_INFO("Client: TCP connection established");

                // Create TCP stream.
                auto tcp_stm = std::make_shared<streaming::tcp::stream>(std::move(client), scheduler);

                // Create TCP transport
                auto client_transport = rpc::stream_transport::transport::make_client(
                    "client_transport", client_service, std::move(tcp_stm));

                RPC_INFO("Client: Starting RPC connection...");

                auto error = CO_AWAIT client_service->connect_to_zone(
                    "tcp_server", client_transport, rpc::shared_ptr<i_calculator>(), remote_calculator);

                if (error != rpc::error::OK())
                {
                    RPC_ERROR("Client: Failed to connect to zone: {}", static_cast<int>(error));
                    CO_RETURN false;
                }

                RPC_INFO("Client: RPC connection established");
            }
            // now make rpc calls
            {
                RPC_INFO("Client: Making remote RPC calls...");

                int result;
                [[maybe_unused]] auto error = CO_AWAIT remote_calculator->add(100, 200, result);
                RPC_INFO("Client: add(100, 200) = {} (error: {})", result, static_cast<int>(error));

                error = CO_AWAIT remote_calculator->multiply(7, 8, result);
                RPC_INFO("Client: multiply(7, 8) = {} (error: {})", result, static_cast<int>(error));

                error = CO_AWAIT remote_calculator->subtract(500, 200, result);
                RPC_INFO("Client: subtract(500, 200) = {} (error: {})", result, static_cast<int>(error));

                remote_calculator.reset();
                RPC_INFO("Client: Released remote calculator");

                client_finished.set();
            }

            print_separator("TCP CLIENT SHUTDOWN");
            CO_RETURN true;
        }
    }
}

void rpc_log(int level, const char* str, size_t sz)
{
    std::string message(str, sz);
    switch (level)
    {
    case 0:
        printf("[TRACE] %s\n", message.c_str());
        break;
    case 1:
        printf("[DEBUG] %s\n", message.c_str());
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

int main(int argc, char* argv[])
{
    RPC_INFO("RPC++ Comprehensive Demo - TCP Transport");
    RPC_INFO("========================================");

    rpc::zone_address server_zone_addr;
    rpc::zone_address client_zone_addr;
    canopy::network_config::network_config cfg;

    {
        args::ArgumentParser parser("TCP transport demo: client/server RPC over TCP.");
        args::HelpFlag help(parser, "help", "Display this help message and exit", {'h', "help"});

        // All network options (including --host and --port) are registered by add_network_args.
        auto net = canopy::network_config::add_network_args(parser);

        try
        {
            parser.ParseCLI(argc, argv);
        }
        catch (const args::Help&)
        {
            std::cout << parser;
            return 0;
        }
        catch (const args::ParseError& e)
        {
            std::cerr << e.what() << "\n" << parser; // to stderr before logging is running
            return 1;
        }

        try
        {
            cfg = net.get_config();
        }
        catch (const std::invalid_argument& e)
        {
            RPC_ERROR("Configuration error: {}", e.what());
            return 1;
        }

        // Require an explicit port for the demo; default to 18888 when not specified.
        if (cfg.port == 0)
            cfg.port = 18888;

        cfg.log_values();

        // Build the zone allocator for this process from the parsed network config.
        auto allocator = canopy::network_config::make_allocator(cfg);

        // Pre-allocate zone addresses for server and client so both sides can refer to
        // stable zone IDs across all iterations.
        allocator.allocate_zone(server_zone_addr);
        allocator.allocate_zone(client_zone_addr);
    }

    auto scheduler_1 = std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
        coro::scheduler::options{.thread_strategy = coro::scheduler::thread_strategy_t::spawn,
            .pool = coro::thread_pool::options{.thread_count = 4},
            .execution_strategy = coro::scheduler::execution_strategy_t::process_tasks_on_thread_pool}));

    auto scheduler_2 = std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
        coro::scheduler::options{.thread_strategy = coro::scheduler::thread_strategy_t::spawn,
            .pool = coro::thread_pool::options{.thread_count = 4},
            .execution_strategy = coro::scheduler::execution_strategy_t::process_tasks_on_thread_pool}));

    for (int i = 0; i < 100; i++)
    {
        RPC_INFO("\n=== Test iteration {} ===", i + 1);

        rpc::event server_ready;
        rpc::event client_finished;

        coro::sync_wait(coro::when_all(
            // the server
            comprehensive::v1::run_tcp_server(scheduler_1, server_ready, client_finished, cfg, rpc::zone{server_zone_addr}),
            // the client
            comprehensive::v1::run_tcp_client(
                scheduler_2, server_ready, client_finished, cfg, rpc::zone{client_zone_addr})));
    }

    print_separator("TCP TRANSPORT DEMO COMPLETED");
    scheduler_1->shutdown();
    scheduler_2->shutdown();
    return 0;
}
