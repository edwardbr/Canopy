// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#include "demo_zone.h"
#include "http_client_connection.h"
#include "rest_echo_service.h"
#include "websocket_handler.h"

#include <csignal>
#include <cstdlib>
#include <cstddef>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <canopy/network_config/cli_args.h>
#include <canopy/network_config/zone.h>
#include <canopy/http_server/http_acceptor.h>
#include <canopy/rest/server.h>
#include <file_system/file_system_manager.h>
#include <rpc/rpc.h>
#include <streaming/secure_stream.h>

#ifdef CANOPY_BUILD_COROUTINE
#  include <coro/coro.hpp>
#  include <io_uring/linux_io_uring_handle.h>
#endif

#ifndef CANOPY_WEBSOCKET_DEMO_STATIC_ROOT
#  define CANOPY_WEBSOCKET_DEMO_STATIC_ROOT "www"
#endif

namespace
{
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): signal handler state must be static storage.
    volatile std::sig_atomic_t g_stop_requested = 0;

    void on_signal(int signal_number)
    {
        if (g_stop_requested != 0)
            std::_Exit(128 + signal_number);
        g_stop_requested = 1;
    }

    auto stop_requested() -> bool
    {
        return g_stop_requested != 0;
    }

    struct augmented_cli
    {
        int argc = 0;
        std::vector<std::string> storage;
        std::vector<char*> argv;
    };

    bool has_cli_option(
        std::span<char*> argv,
        std::string_view option)
    {
        const std::string with_equals = std::string(option) + "=";
        for (std::size_t i = 1; i < argv.size(); ++i)
        {
            const std::string_view arg = argv[i];
            if (arg == option || arg.rfind(with_equals, 0) == 0)
                return true;
        }

        return false;
    }

    augmented_cli add_default_network_args(std::span<char*> argv)
    {
        augmented_cli result;
        result.storage.reserve(8);
        result.argv.reserve(argv.size() + 8);

        for (char* arg : argv)
            result.argv.push_back(arg);

        const bool has_any_va = has_cli_option(argv, "--va-name") || has_cli_option(argv, "--va-type")
                                || has_cli_option(argv, "--va-prefix") || has_cli_option(argv, "--va-subnet-bits")
                                || has_cli_option(argv, "--va-object-id-bits") || has_cli_option(argv, "--va-subnet");
        const bool has_listen = has_cli_option(argv, "--listen");

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

    auto make_executor() -> rpc::executor_ptr
    {
#ifdef CANOPY_BUILD_COROUTINE
        // clang-format off
        return rpc::executor_ptr(coro::scheduler::make_unique(
            coro::scheduler::options{
                FLD(thread_strategy) coro::scheduler::thread_strategy_t::spawn,
                FLD(on_io_thread_start_functor) []
                { RPC_DEBUG("process event thread start"); },
                FLD(on_io_thread_stop_functor) []
                { RPC_DEBUG("io_scheduler::process event thread stop"); },
                FLD(pool) coro::thread_pool::options{
                    FLD(thread_count) std::thread::hardware_concurrency(),
                    FLD(on_thread_start_functor) [](size_t i)
                    {
                        (void)i;
                        RPC_DEBUG("io_scheduler::thread_pool worker {} starting", i);
                    },
                    FLD(on_thread_stop_functor) [](size_t i)
                    {
                        (void)i;
                        RPC_DEBUG("io_scheduler::thread_pool worker {} stopping", i);
                    },
                },
                FLD(execution_strategy) coro::scheduler::execution_strategy_t::process_tasks_on_thread_pool,
            }));
// clang-format onc++/demos/websocket/server/demo_zone.h
#else
        return std::make_shared<rpc::blocking_executor>();
#endif
}

    auto create_static_file_manager(rpc::executor_ptr executor) -> rpc::shared_ptr<rpc::file_system::i_manager>
{
#ifdef CANOPY_BUILD_COROUTINE
    rpc::io_uring::linux_io_uring_handle::options handle_options;
    handle_options.queue_depth = 256;
    handle_options.buffer_count = 64;
    handle_options.buffer_size = 64U * 1024U;
    handle_options.register_fixed_files = true;
    handle_options.fixed_file_count = 256;
    handle_options.use_sqpoll = true;

    std::shared_ptr<rpc::io_uring::linux_io_uring_handle> handle;
    auto error = rpc::io_uring::linux_io_uring_handle::create(handle, handle_options, executor);
    if (error != rpc::error::OK())
    {
        RPC_ERROR("failed to create websocket static file io_uring handle error={}", error);
        return {};
    }

    rpc::io_uring::controller::options controller_options;
    controller_options.completion_wait_strategy = rpc::io_uring::wait_strategy::proactor;
    controller_options.use_caller_buffers_for_transfers = true;

    auto controller = std::make_shared<rpc::io_uring::controller>(std::move(handle), executor.get(), controller_options);
    return rpc::file_system::create_factory(std::move(controller));
#else
        (void)executor;
        return rpc::file_system::create_factory();
#endif
}

    // (Previous async load_tls_credentials helper removed — the
    // streaming::secure::context(cert_path, key_path) constructor reads
    // the PEM files synchronously and works in both modes.)

    struct websocket_stream_handler
    {
        std::shared_ptr<websocket_demo::v1::websocket_service> service;
        rpc::shared_ptr<rpc::file_system::i_manager> file_system_manager;
        canopy::rest::endpoint_registry rest_handlers;
        std::string static_root_path;

        auto operator()(std::shared_ptr<streaming::stream> stream) -> CORO_TASK(std::shared_ptr<rpc::transport>)
        {
            // The demo-specific factory is bound here; http_client_connection only sees the generic upgrade callback.
            auto websocket_handler = websocket_demo::v1::make_websocket_upgrade_handler(service);
            websocket_demo::v1::http_client_connection connection(
                std::move(stream), std::move(websocket_handler), file_system_manager, static_root_path, rest_handlers);

            // service this call
            CO_RETURN CO_AWAIT connection.handle();
        }
    };
}

auto run_http_server(
    rpc::executor_ptr executor,
    canopy::http_server::endpoint ep,
    std::shared_ptr<websocket_demo::v1::websocket_service> service,
    rpc::shared_ptr<rpc::file_system::i_manager> file_system_manager,
    canopy::rest::endpoint_registry rest_handlers,
    std::string static_root_path,
    std::shared_ptr<streaming::secure::context> tls_ctx) -> CORO_TASK(void)
{
    // when a new client connection is made this is called
    auto stream_handler = websocket_stream_handler{
        std::move(service), std::move(file_system_manager), std::move(rest_handlers), std::move(static_root_path)};

    CO_AWAIT canopy::http_server::run_server(
        ep, executor, std::move(stream_handler), tls_ctx, [] { return stop_requested(); });
    CO_RETURN;
}

auto main(
    int argc,
    char** argv) -> int
try
{
    args::ArgumentParser parser("WebSocket demo server with static pages, REST endpoints, and websocket RPC.");
    args::HelpFlag help(parser, "help", "Display this help message and exit", {'h', "help"});
    auto net = canopy::network_config::add_network_args(parser);
    args::ValueFlag<std::string> cert_file(parser, "file", "Path to TLS certificate file (PEM format)", {"cert"}, "");
    args::ValueFlag<std::string> key_file(parser, "file", "Path to TLS private key file (PEM format)", {"key"}, "");
    args::ValueFlag<std::string> path(
        parser, "path", "Path to static websocket demo files", {"static-root"}, CANOPY_WEBSOCKET_DEMO_STATIC_ROOT);
    auto cli = add_default_network_args(std::span<char*>(argv, static_cast<std::size_t>(argc)));

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

    // Resolve the listen endpoint. Default: bind on 127.0.0.1:8080 via add_default_network_args().
    canopy::network_config::tcp_endpoint listen_ep;
    if (const auto* p = cfg.first_listen())
        listen_ep = *p;
    else
        listen_ep.port = 8080; // addr = {} = 0.0.0.0, family = ipv4

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);

    const auto cert_path = args::get(cert_file);
    const auto key_path = args::get(key_file);
    const auto static_root_path = args::get(path);
    if (static_root_path.empty())
    {
        RPC_ERROR("--static-root must not be empty");
        return 1;
    }

    auto executor = make_executor();
    auto file_system_manager = create_static_file_manager(executor);
    if (!file_system_manager)
    {
        executor->shutdown();
        return 1;
    }

    std::shared_ptr<streaming::secure::context> tls_ctx;
    if (!cert_path.empty() && !key_path.empty())
    {
        // Direct file-load path works in both modes; file_system_manager is
        // dual-mode and streaming::secure::context only needs the PEM bytes.
        tls_ctx = std::make_shared<streaming::secure::context>(cert_path, key_path);
        if (!tls_ctx->is_valid())
        {
            RPC_ERROR("Failed to initialize TLS context, exiting");
            executor->shutdown();
            return 1;
        }
        RPC_INFO("TLS enabled with certificate: {}", cert_path);
    }
    else if (!cert_path.empty() || !key_path.empty())
    {
        RPC_ERROR("Both --cert and --key must be provided for TLS");
        executor->shutdown();
        return 1;
    }

    auto address = canopy::network_config::get_zone_address(cfg);
    std::ignore = address.set_subnet(1);

    auto root_service = std::make_shared<websocket_demo::v1::websocket_service>("demo", address, executor);
    canopy::rest::endpoint_registry rest_handlers;
    rest_handlers.add_object("echo", websocket_demo::v1::make_echo_service(), "/api");

    canopy::http_server::endpoint http_ep;
    http_ep.host = listen_ep.to_string();
    http_ep.port = listen_ep.port;
    http_ep.ipv6 = listen_ep.family == canopy::network_config::ip_address_family::ipv6;

    // SYNC_WAIT collapses to coro::sync_wait in coroutine builds and to the
    // raw expression in blocking builds (where run_http_server returns void
    // and runs the accept loop synchronously on this thread).
    SYNC_WAIT(run_http_server(
        executor, http_ep, root_service, file_system_manager, std::move(rest_handlers), static_root_path, tls_ctx));
    root_service.reset();
    executor->shutdown();
    return 0;
}
catch (...)
{
    return 1;
}
