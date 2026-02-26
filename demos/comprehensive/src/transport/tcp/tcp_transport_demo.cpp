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
 *     --subnet-base <n>     First subnet ID to allocate (default: 1 for local demos)
 *     --subnet-range <n>    Number of subnets reserved (default: max)
 */

#include <demo_impl.h>
#include <rpc/rpc.h>
#include <iostream> // kept for args help/error output only
#include <chrono>

#include <transports/tcp/transport.h>
#include <transports/tcp/listener.h>
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

#ifdef CANOPY_BUILD_COROUTINE
        CORO_TASK(bool)
        run_tcp_server(std::shared_ptr<coro::io_scheduler> scheduler,
            rpc::event& server_ready,
            const rpc::event& client_finished,
            const canopy::network_config::network_config& cfg)
        {
            rpc::zone_address zone_addr(
                canopy::network_config::ip_address_to_uint64(cfg.routing_prefix_addr, cfg.routing_prefix_family),
                static_cast<uint32_t>(cfg.subnet_base));
            std::string host = cfg.get_host_string();
            uint16_t port = cfg.port;

            print_separator("TCP SERVER (Coroutine Mode)");

            auto on_shutdown_event = std::make_shared<rpc::event>();

            // Create server service using the zone address provided by the allocator.
            auto service = std::make_shared<rpc::service>("tcp_server", rpc::zone{zone_addr}, scheduler);
            service->set_shutdown_event(on_shutdown_event);

            RPC_INFO("Server zone ID (address): {}", rpc::to_yas_json<std::string>(service->get_zone_id().get_address()));

            // Create TCP listener with connection handler.
            auto listener = std::make_shared<rpc::tcp::listener>(
                [](const rpc::connection_settings& input_descr,
                    rpc::interface_descriptor& output_interface,
                    std::shared_ptr<rpc::service> child_service_ptr,
                    std::shared_ptr<rpc::tcp::tcp_transport> transport) -> CORO_TASK(int)
                {
                    RPC_INFO("Server: Accepting connection from zone {}", input_descr.input_zone_id.get_subnet());

                    auto ret
                        = CO_AWAIT child_service_ptr->attach_remote_zone<i_calculator, i_calculator>("tcp_client_proxy",
                            transport,
                            input_descr,
                            output_interface,
                            [](const rpc::shared_ptr<i_calculator>& remote_calc,
                                rpc::shared_ptr<i_calculator>& local_calc,
                                const std::shared_ptr<rpc::service>& service_ptr) -> CORO_TASK(int)
                            {
                                local_calc = rpc::shared_ptr<i_calculator>(new calculator_impl(service_ptr));
                                RPC_INFO("Server: Created calculator service");
                                CO_RETURN rpc::error::OK();
                            });

                    if (ret == rpc::error::OK())
                        RPC_INFO("Server: Client connected successfully");
                    else
                        RPC_ERROR("Server: Client connection failed: {}", static_cast<int>(ret));

                    CO_RETURN ret;
                },
                std::chrono::milliseconds(100000));

            auto server_options = coro::net::tcp::server::options{
                .address = coro::net::ip_address::from_string(host,
                    cfg.host_family == canopy::network_config::ip_address_family::ipv6 ? coro::net::domain_t::ipv6
                                                                                       : coro::net::domain_t::ipv4),
                .port = port,
                .backlog = 10};

            if (!listener->start_listening(service, server_options))
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
        run_tcp_client(std::shared_ptr<coro::io_scheduler> scheduler,
            const rpc::event& server_ready,
            rpc::event& client_finished,
            const canopy::network_config::network_config& cfg)
        {
            rpc::zone client_zone(rpc::zone_address{
                canopy::network_config::ip_address_to_uint64(cfg.routing_prefix_addr, cfg.routing_prefix_family) + 1,
                static_cast<uint32_t>(cfg.subnet_base)});
            rpc::zone server_zone(rpc::zone_address{
                canopy::network_config::ip_address_to_uint64(cfg.routing_prefix_addr, cfg.routing_prefix_family),
                static_cast<uint32_t>(cfg.subnet_base)});
            std::string host = cfg.get_host_string();
            uint16_t port = cfg.port;

            print_separator("TCP CLIENT (Coroutine Mode)");
            {
                // Wait for server to be ready before connecting.
                co_await server_ready.wait();

                // Create client service using the zone address provided by the allocator.
                auto client_service = std::make_shared<rpc::service>("tcp_client", client_zone, scheduler);

                RPC_INFO("Client zone ID (address): {}",
                    rpc::to_yas_json<std::string>(client_service->get_zone_id().get_address()));

                RPC_INFO("Client: Connecting to {}:{}...", host, port);

                // Create TCP client.
                coro::net::tcp::client client(scheduler,
                    coro::net::tcp::client::options{.address = coro::net::ip_address::from_string(host,
                                                        cfg.host_family == canopy::network_config::ip_address_family::ipv6
                                                            ? coro::net::domain_t::ipv6
                                                            : coro::net::domain_t::ipv4),
                        .port = port});

                auto connection_status = CO_AWAIT client.connect(std::chrono::milliseconds(5000));
                if (connection_status != coro::net::connect_status::connected)
                {
                    RPC_ERROR("Client: Failed to connect to server (status: {})", static_cast<int>(connection_status));
                    CO_RETURN false;
                }

                RPC_INFO("Client: TCP connection established");

                // Create TCP transport — server_zone identifies the remote zone.
                auto client_transport = rpc::tcp::tcp_transport::create("client_transport",
                    client_service,
                    server_zone,
                    std::chrono::milliseconds(100000),
                    std::move(client),
                    nullptr);

                // Start the pump before connect_to_zone.
                client_service->spawn(client_transport->pump_send_and_receive());

                RPC_INFO("Client: Starting RPC connection...");

                rpc::shared_ptr<i_calculator> local_calculator;
                rpc::shared_ptr<i_calculator> remote_calculator;

                auto error = CO_AWAIT client_service->connect_to_zone(
                    "tcp_server", client_transport, local_calculator, remote_calculator);

                if (error != rpc::error::OK())
                {
                    RPC_ERROR("Client: Failed to connect to zone: {}", static_cast<int>(error));
                    CO_RETURN false;
                }

                RPC_INFO("Client: RPC connection established");

                client_finished.set();

                RPC_INFO("Client: Making remote RPC calls...");

                int result;
                error = CO_AWAIT remote_calculator->add(100, 200, result);
                RPC_INFO("Client: add(100, 200) = {} (error: {})", result, static_cast<int>(error));

                error = CO_AWAIT remote_calculator->multiply(7, 8, result);
                RPC_INFO("Client: multiply(7, 8) = {} (error: {})", result, static_cast<int>(error));

                error = CO_AWAIT remote_calculator->subtract(500, 200, result);
                RPC_INFO("Client: subtract(500, 200) = {} (error: {})", result, static_cast<int>(error));

                remote_calculator.reset();
                RPC_INFO("Client: Released remote calculator");
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

int main(int argc, char* argv[])
{
    RPC_INFO("RPC++ Comprehensive Demo - TCP Transport");
    RPC_INFO("========================================");

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

    canopy::network_config::network_config cfg;
    try
    {
        cfg = net.get_config();
    }
    catch (const std::invalid_argument& e)
    {
        RPC_ERROR("Configuration error: {}", e.what());
        return 1;
    }

    // subnet_id == 0 with no routing prefix is the "unset" sentinel in zone_address::is_set().
    // For local demos without a network routing prefix, reserve subnet IDs from 1 onwards.
    if (!cfg.has_routing_prefix() && cfg.subnet_base == 0)
        cfg.subnet_base = 1;

    // Require an explicit port for the demo; default to 18888 when not specified.
    if (cfg.port == 0)
        cfg.port = 18888;

    // Build the zone allocator for this process from the parsed network config.
    // In local-only mode (no routing prefix) this behaves like the old zone_gen counter.
    // In network mode, routing_prefix encodes the host's IP address so that zone addresses
    // are globally unique without any coordination with other nodes.
    // auto allocator = canopy::network_config::make_allocator(cfg);

    // Pre-allocate zone addresses for server and client so both sides can refer to the
    // server's zone before the TCP connection is established.
    // auto server_zone_addr = allocator.allocate_zone();
    // auto client_zone_addr = allocator.allocate_zone();

    const std::string host = cfg.get_host_string();

    cfg.log_values();
    // RPC_INFO("Server subnet  : {}", server_zone_addr.get_subnet());
    // RPC_INFO("Client subnet  : {}", client_zone_addr.get_subnet());

#ifndef CANOPY_BUILD_COROUTINE
    RPC_ERROR("TCP transport requires coroutines. Please configure with: cmake --preset Coroutine_Debug");
    return 1;
#else

    auto scheduler_1 = coro::io_scheduler::make_shared(
        coro::io_scheduler::options{.thread_strategy = coro::io_scheduler::thread_strategy_t::spawn,
            .pool = coro::thread_pool::options{.thread_count = 4},
            .execution_strategy = coro::io_scheduler::execution_strategy_t::process_tasks_on_thread_pool});

    auto scheduler_2 = coro::io_scheduler::make_shared(
        coro::io_scheduler::options{.thread_strategy = coro::io_scheduler::thread_strategy_t::spawn,
            .pool = coro::thread_pool::options{.thread_count = 4},
            .execution_strategy = coro::io_scheduler::execution_strategy_t::process_tasks_on_thread_pool});

    for (int i = 0; i < 100; i++)
    {
        RPC_INFO("\n=== Test iteration {} ===", i + 1);

        rpc::event server_ready;
        rpc::event client_finished;

        coro::sync_wait(coro::when_all(comprehensive::v1::run_tcp_server(scheduler_1, server_ready, client_finished, cfg),
            comprehensive::v1::run_tcp_client(scheduler_2, server_ready, client_finished, cfg)));
    }

    print_separator("TCP TRANSPORT DEMO COMPLETED");
    scheduler_1->shutdown();
    scheduler_2->shutdown();
    return 0;
#endif
}
