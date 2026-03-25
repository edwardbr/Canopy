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
 *   1. cmake --preset Debug_Coroutine
 *   2. cmake --build build_debug_coroutine --target tcp_transport_demo
 *   3. ./build_debug_coroutine/output/debug/demos/comprehensive/tcp_transport_demo [options]
 *
 *   Options:
 *     --va-name   <name>       Virtual address name (e.g. "demo")
 *     --va-type   <type>       Zone address type: local | ipv4 | ipv6 | ipv6_tun
 *     --va-prefix <prefix>     Routing prefix (auto-detected if omitted)
 *     --listen    [name:]addr:port   Server bind address (e.g. "demo:0.0.0.0:18888")
 *     --connect   [name:]addr:port   Client connect address (e.g. "demo:127.0.0.1:18888")
 *     Address family is inferred from the address format ([::] = IPv6, a.b.c.d = IPv4).
 */

#include <demo_impl.h>
#include <rpc/rpc.h>
#include <iostream>
#include <chrono>
#include <string_view>
#include <vector>

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

namespace
{
    struct augmented_cli
    {
        int argc = 0;
        std::vector<std::string> storage;
        std::vector<char*> argv;
    };

    bool has_cli_option(
        int argc,
        char* argv[],
        std::string_view option)
    {
        const std::string with_equals = std::string(option) + "=";
        for (int i = 1; i < argc; ++i)
        {
            const std::string_view arg = argv[i];
            if (arg == option || arg.rfind(with_equals, 0) == 0)
                return true;
        }

        return false;
    }

    augmented_cli add_default_network_args(
        int argc,
        char* argv[])
    {
        augmented_cli result;
        result.storage.reserve(16);
        result.argv.reserve(argc + 16);

        for (int i = 0; i < argc; ++i)
            result.argv.push_back(argv[i]);

        const bool has_any_va
            = has_cli_option(argc, argv, "--va-name") || has_cli_option(argc, argv, "--va-type")
              || has_cli_option(argc, argv, "--va-prefix") || has_cli_option(argc, argv, "--va-subnet-bits")
              || has_cli_option(argc, argv, "--va-object-id-bits") || has_cli_option(argc, argv, "--va-subnet");
        const bool has_listen = has_cli_option(argc, argv, "--listen");
        const bool has_connect = has_cli_option(argc, argv, "--connect");

        auto append = [&result](std::initializer_list<const char*> args)
        {
            for (const char* arg : args)
            {
                result.storage.emplace_back(arg);
                result.argv.push_back(result.storage.back().data());
            }
        };

        if (!has_any_va)
        {
            append(
                {"--va-name=server",
                    "--va-type=ipv4",
                    "--va-prefix=127.0.0.1",
                    "--va-subnet-bits=32",
                    "--va-object-id-bits=32",
                    "--va-subnet=1",
                    "--va-name=client",
                    "--va-type=ipv4",
                    "--va-prefix=127.0.0.1",
                    "--va-subnet-bits=32",
                    "--va-object-id-bits=32",
                    "--va-subnet=100"});
        }

        if (!has_listen)
            append({"--listen=server:127.0.0.1:8080"});

        if (!has_connect)
            append({"--connect=client:127.0.0.1:8080"});

        result.argc = static_cast<int>(result.argv.size());
        return result;
    }
}

namespace comprehensive
{
    namespace v1
    {
        CORO_TASK(bool)
        run_tcp_server(
            std::shared_ptr<coro::scheduler> scheduler,
            rpc::event& server_ready,
            const rpc::event& client_finished,
            const canopy::network_config::tcp_endpoint& listen_ep,
            rpc::zone server_zone)
        {
            const std::string host = listen_ep.to_string();
            const uint16_t port = listen_ep.port;

            print_separator("TCP SERVER (Coroutine Mode)");

            auto on_shutdown_event = std::make_shared<rpc::event>();

            auto service = std::make_shared<rpc::root_service>("tcp_server", server_zone, scheduler);
            service->set_shutdown_event(on_shutdown_event);

            RPC_INFO("Server zone ID (address): {}", rpc::to_yas_json<std::string>(service->get_zone_id().get_address()));

            const auto domain = listen_ep.family == canopy::network_config::ip_address_family::ipv6
                                    ? coro::net::domain_t::ipv6
                                    : coro::net::domain_t::ipv4;
            const coro::net::socket_address endpoint{coro::net::ip_address::from_string(host, domain), port};

            auto listener = std::make_shared<streaming::listener>(
                "server_transport",
                std::make_shared<streaming::tcp::acceptor>(endpoint),
                rpc::stream_transport::make_connection_callback<i_calculator, i_calculator>(
                    [](const rpc::shared_ptr<i_calculator>&,
                        const std::shared_ptr<rpc::service>& svc) -> CORO_TASK(rpc::service_connect_result<i_calculator>)
                    {
                        CO_RETURN rpc::service_connect_result<i_calculator>{
                            rpc::error::OK(), rpc::shared_ptr<i_calculator>(new calculator_impl(svc))};
                    }));

            if (!listener->start_listening(service))
            {
                RPC_ERROR("Server: Failed to start listening");
                CO_RETURN false;
            }

            RPC_INFO("Server: Listening on {}:{}", host, port);
            service.reset();
            server_ready.set();

            co_await client_finished.wait();

            co_await listener->stop_listening();
            listener.reset();

            co_await on_shutdown_event->wait();

            print_separator("TCP SERVER SHUTDOWN");
            CO_RETURN true;
        }

        CORO_TASK(bool)
        run_tcp_client(
            std::shared_ptr<coro::scheduler> scheduler,
            const rpc::event& server_ready,
            rpc::event& client_finished,
            const canopy::network_config::tcp_endpoint& connect_ep,
            rpc::zone client_zone)
        {
            const std::string host = connect_ep.to_string();
            const uint16_t port = connect_ep.port;

            auto client_service = std::make_shared<rpc::root_service>("tcp_client", client_zone, scheduler);

            rpc::shared_ptr<i_calculator> remote_calculator;

            {
                co_await server_ready.wait();

                RPC_INFO(
                    "Client zone ID (address): {}",
                    rpc::to_yas_json<std::string>(client_service->get_zone_id().get_address()));

                RPC_INFO("Client: Connecting to {}:{}...", host, port);
            }

            {
                const auto client_domain = connect_ep.family == canopy::network_config::ip_address_family::ipv6
                                               ? coro::net::domain_t::ipv6
                                               : coro::net::domain_t::ipv4;
                coro::net::tcp::client client(
                    scheduler, coro::net::socket_address{coro::net::ip_address::from_string(host, client_domain), port});

                auto connection_status = CO_AWAIT client.connect(std::chrono::milliseconds(5000));
                if (connection_status != coro::net::connect_status::connected)
                {
                    RPC_ERROR("Client: Failed to connect to server (status: {})", static_cast<int>(connection_status));
                    CO_RETURN false;
                }

                RPC_INFO("Client: TCP connection established");

                auto tcp_stm = std::make_shared<streaming::tcp::stream>(std::move(client), scheduler);
                auto client_transport
                    = rpc::stream_transport::make_client("client_transport", client_service, std::move(tcp_stm));

                RPC_INFO("Client: Starting RPC connection...");

                auto connect_result = CO_AWAIT client_service->connect_to_zone<i_calculator, i_calculator>(
                    "tcp_server", client_transport, rpc::shared_ptr<i_calculator>());
                remote_calculator = connect_result.output_interface;
                auto error = connect_result.error_code;

                if (error != rpc::error::OK())
                {
                    RPC_ERROR("Client: Failed to connect to zone: {}", static_cast<int>(error));
                    CO_RETURN false;
                }

                RPC_INFO("Client: RPC connection established");
            }

            {
                RPC_INFO("Client: Making remote RPC calls...");

                int result = 0;
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

void rpc_log(
    int level,
    const char* str,
    size_t sz)
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

int main(
    int argc,
    char* argv[])
{
    RPC_INFO("RPC++ Comprehensive Demo - TCP Transport");
    RPC_INFO("========================================");

    rpc::zone_address server_zone_addr;
    rpc::zone_address client_zone_addr;
    canopy::network_config::tcp_endpoint listen_ep;
    canopy::network_config::tcp_endpoint connect_ep;

    {
        args::ArgumentParser parser("TCP transport demo: client/server RPC over TCP.");
        args::HelpFlag help(parser, "help", "Display this help message and exit", {'h', "help"});

        auto net = canopy::network_config::add_network_args(parser);
        auto cli = add_default_network_args(argc, argv);

        try
        {
            parser.ParseCLI(cli.argc, cli.argv.data());
        }
        catch (const args::Help&)
        {
            std::cout << parser;
            return 0;
        }
        catch (const args::ParseError& e)
        {
            std::cerr << e.what() << "\n" << parser;
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

        cfg.log_values();

        // Resolve the listen endpoint.  Default: bind on 0.0.0.0:18888.
        if (const auto* p = cfg.first_listen())
            listen_ep = *p;
        else
            listen_ep.port = 18888; // addr = {} = 0.0.0.0, family = ipv4

        // Resolve the connect endpoint.  Default: connect to 127.0.0.1 on the
        // same port as the listen endpoint (in-process demo pattern).
        if (const auto* p = cfg.first_connect())
        {
            connect_ep = *p;
        }
        else
        {
            connect_ep = listen_ep;
            // Replace an all-zeros listen address with loopback for the client.
            const bool is_any
                = std::all_of(connect_ep.addr.begin(), connect_ep.addr.end(), [](uint8_t b) { return b == 0; });
            if (is_any)
                canopy::network_config::ipv4_to_ip_address("127.0.0.1", connect_ep.addr);
        }

        // Build zone allocator from virtual address config (auto-detects prefix if --va-prefix omitted).
        auto allocator = canopy::network_config::make_allocator(cfg);
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

        coro::sync_wait(
            coro::when_all(
                comprehensive::v1::run_tcp_server(
                    scheduler_1, server_ready, client_finished, listen_ep, rpc::zone{server_zone_addr}),
                comprehensive::v1::run_tcp_client(
                    scheduler_2, server_ready, client_finished, connect_ep, rpc::zone{client_zone_addr})));
    }

    print_separator("TCP TRANSPORT DEMO COMPLETED");
    scheduler_1->shutdown();
    scheduler_2->shutdown();
    return 0;
}
