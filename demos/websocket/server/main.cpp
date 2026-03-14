// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#include "http_client_connection.h"

#include <csignal>
#include <iostream>
#include <memory>
#include <string>

#include <canopy/network_config/network_args.h>
#include <canopy/http_server/http_acceptor.h>
#include <coro/coro.hpp>
#include <rpc/rpc.h>
#include <streaming/tls_stream.h>

extern "C" void rpc_log(int level, const char* str, size_t sz)
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

namespace
{
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

auto run_http_server(std::shared_ptr<coro::scheduler> scheduler,
    coro::net::ip_address bind_address,
    uint16_t port,
    std::shared_ptr<websocket_demo::v1::websocket_service> service,
    std::shared_ptr<streaming::tls_context> tls_ctx) -> coro::task<void>
{
    auto stream_handler
        = [service](std::shared_ptr<streaming::stream> stream) -> coro::task<std::shared_ptr<rpc::transport>>
    {
        websocket_demo::v1::http_client_connection connection(std::move(stream), service);
        co_return CO_AWAIT connection.handle();
    };

    co_return CO_AWAIT canopy::http_server::run_server(
        std::move(bind_address), port, scheduler, std::move(stream_handler), tls_ctx);
}

auto main(int argc, char* argv[]) -> int
{
    args::ArgumentParser parser("WebSocket demo server with static pages, REST endpoints, and websocket RPC.");
    args::HelpFlag help(parser, "help", "Display this help message and exit", {'h', "help"});
    auto net = canopy::network_config::add_network_args(parser);
    args::ValueFlag<std::string> cert_file(parser, "file", "Path to TLS certificate file (PEM format)", {"cert"}, "");
    args::ValueFlag<std::string> key_file(parser, "file", "Path to TLS private key file (PEM format)", {"key"}, "");

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

    if (cfg.port == 0)
    {
        cfg.port = 8888;
    }

    cfg.log_values();

    const auto cert_path = args::get(cert_file);
    const auto key_path = args::get(key_file);
    std::shared_ptr<streaming::tls_context> tls_ctx;
    if (!cert_path.empty() && !key_path.empty())
    {
        tls_ctx = std::make_shared<streaming::tls_context>(cert_path, key_path);
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
    address.set_subnet(1);

    auto root_service = std::make_shared<websocket_demo::v1::websocket_service>("demo", address, scheduler);
    const auto domain = cfg.host_family == canopy::network_config::ip_address_family::ipv6 ? coro::net::domain_t::ipv6
                                                                                           : coro::net::domain_t::ipv4;
    auto bind_address = coro::net::ip_address::from_string(cfg.get_host_string(), domain);

    coro::sync_wait(coro::when_all(run_http_server(scheduler, std::move(bind_address), cfg.port, root_service, tls_ctx)));
}
