// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#include "http_client_connection.h"

#include <csignal>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <canopy/network_config/network_args.h>
#include <canopy/http_server/http_acceptor.h>
#include <coro/coro.hpp>
#include <rpc/rpc.h>
#include <streaming/tls/stream.h>

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
        result.storage.reserve(8);
        result.argv.reserve(argc + 8);

        for (int i = 0; i < argc; ++i)
            result.argv.push_back(argv[i]);

        const bool has_any_va
            = has_cli_option(argc, argv, "--va-name") || has_cli_option(argc, argv, "--va-type")
              || has_cli_option(argc, argv, "--va-prefix") || has_cli_option(argc, argv, "--va-subnet-bits")
              || has_cli_option(argc, argv, "--va-object-id-bits") || has_cli_option(argc, argv, "--va-subnet");
        const bool has_listen = has_cli_option(argc, argv, "--listen");

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
                    "--va-subnet=1"});
        }

        if (!has_listen)
            append({"--listen=server:127.0.0.1:8080"});

        result.argc = static_cast<int>(result.argv.size());
        return result;
    }

    auto make_scheduler() -> std::shared_ptr<coro::scheduler>
    {
        return std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
            coro::scheduler::options{
                .thread_strategy = coro::scheduler::thread_strategy_t::spawn,
                .on_io_thread_start_functor = []
                { RPC_DEBUG("process event thread start"); },
                .on_io_thread_stop_functor = []
                { RPC_DEBUG("io_scheduler::process event thread stop"); },
                .pool = coro::thread_pool::options{
                    .thread_count = std::thread::hardware_concurrency(),
                    .on_thread_start_functor = [](size_t i)
                    { RPC_DEBUG("io_scheduler::thread_pool worker {} starting", i); },
                    .on_thread_stop_functor = [](size_t i)
                    { RPC_DEBUG("io_scheduler::thread_pool worker {} stopping", i); },
                },
                .execution_strategy = coro::scheduler::execution_strategy_t::process_tasks_on_thread_pool,
            }));
    }
}

auto run_http_server(
    std::shared_ptr<coro::scheduler> scheduler,
    coro::net::ip_address bind_address,
    uint16_t port,
    std::shared_ptr<websocket_demo::v1::websocket_service> service,
    std::shared_ptr<streaming::tls::context> tls_ctx) -> coro::task<void>
{
    auto stream_handler
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
        = [service](std::shared_ptr<streaming::stream> stream) -> coro::task<std::shared_ptr<rpc::transport>>
    {
        websocket_demo::v1::http_client_connection connection(std::move(stream), service);
        co_return CO_AWAIT connection.handle();
    };

    CO_AWAIT canopy::http_server::run_server(bind_address, port, scheduler, std::move(stream_handler), tls_ctx);
    co_return;
}

auto main(
    int argc,
    char* argv[]) -> int
{
    args::ArgumentParser parser("WebSocket demo server with static pages, REST endpoints, and websocket RPC.");
    args::HelpFlag help(parser, "help", "Display this help message and exit", {'h', "help"});
    auto net = canopy::network_config::add_network_args(parser);
    args::ValueFlag<std::string> cert_file(parser, "file", "Path to TLS certificate file (PEM format)", {"cert"}, "");
    args::ValueFlag<std::string> key_file(parser, "file", "Path to TLS private key file (PEM format)", {"key"}, "");
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

    // Resolve the listen endpoint.  Default: bind on 0.0.0.0:8888.
    canopy::network_config::tcp_endpoint listen_ep;
    if (const auto* p = cfg.first_listen())
        listen_ep = *p;
    else
        listen_ep.port = 8888; // addr = {} = 0.0.0.0, family = ipv4

    const auto cert_path = args::get(cert_file);
    const auto key_path = args::get(key_file);
    std::shared_ptr<streaming::tls::context> tls_ctx;
    if (!cert_path.empty() && !key_path.empty())
    {
        tls_ctx = std::make_shared<streaming::tls::context>(cert_path, key_path);
        if (!tls_ctx->is_valid())
        {
            RPC_ERROR("Failed to initialize TLS context, exiting");
            return 1;
        }
        RPC_INFO("TLS enabled with certificate: {}", cert_path);
    }
    else if (!cert_path.empty() || !key_path.empty())
    {
        RPC_ERROR("Both --cert and --key must be provided for TLS");
        return 1;
    }

    std::signal(SIGPIPE, SIG_IGN);

    auto scheduler = make_scheduler();

    auto address = canopy::network_config::get_zone_address(cfg);
    std::ignore = address.set_subnet(1);

    auto root_service = std::make_shared<websocket_demo::v1::websocket_service>("demo", address, scheduler);

    const auto domain = listen_ep.family == canopy::network_config::ip_address_family::ipv6 ? coro::net::domain_t::ipv6
                                                                                            : coro::net::domain_t::ipv4;
    auto bind_address = coro::net::ip_address::from_string(listen_ep.to_string(), domain);

    coro::sync_wait(coro::when_all(run_http_server(scheduler, bind_address, listen_ep.port, root_service, tls_ctx)));
}
