// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#include <csignal>
#include <cstdlib>
#include <cstdint>
#include <exception>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <vector>

#include <canopy/network_config/network_args.h>
#include <coro/coro.hpp>
#include <io_uring/host_controller.h>
#include <rpc/rpc.h>
#include <transports/sgx_coroutine/host/connect.h>
#include <transports/sgx_coroutine/host/transport.h>
#include <websocket_demo/websocket_demo.h>

#ifndef CANOPY_WEBSOCKET_DEMO_ENCLAVE_PATH
#  error "CANOPY_WEBSOCKET_DEMO_ENCLAVE_PATH must be defined for websocket_enclave_server"
#endif
#ifndef CANOPY_WEBSOCKET_DEMO_STATIC_ROOT
#  define CANOPY_WEBSOCKET_DEMO_STATIC_ROOT "www"
#endif

namespace
{
    volatile std::sig_atomic_t g_stop_requested = 0;

    struct command_line
    {
        uint16_t port{8080};
        std::string listen_address{"127.0.0.1"};
        std::string certificate_path;
        std::string private_key_path;
        std::string static_root_path;
        uint32_t enclave_worker_threads{0};
        std::map<std::string, std::string> enclave_options;
    };

    void on_signal(int signal_number)
    {
        // The repeated-signal path is inside an async signal handler; use the
        // signal-safe hard exit rather than running C++ cleanup machinery here.
        if (g_stop_requested != 0)
            std::_Exit(128 + signal_number);
        g_stop_requested = 1;
    }

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
        for (int index = 1; index < argc; ++index)
        {
            const std::string_view arg = argv[index];
            const std::string with_equals = std::string(option) + "=";
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

        for (int index = 0; index < argc; ++index)
            result.argv.push_back(argv[index]);

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

    auto parse_command_line(
        int argc,
        char* argv[]) -> command_line
    {
        args::ArgumentParser parser("WebSocket enclave demo server.");
        args::HelpFlag help(parser, "help", "Display this help message and exit", {'h', "help"});
        auto net = canopy::network_config::add_network_args(parser);
        args::ValueFlag<std::string> cert_file(
            parser, "file", "Path to TLS certificate file (PEM format); provide with --key to enable TLS", {"cert"}, "");
        args::ValueFlag<std::string> key_file(
            parser, "file", "Path to TLS private key file (PEM format); provide with --cert to enable TLS", {"key"}, "");
        args::ValueFlag<std::string> static_root(
            parser, "path", "Path to static websocket demo files", {"static-root"}, CANOPY_WEBSOCKET_DEMO_STATIC_ROOT);
        args::ValueFlag<uint32_t> enclave_worker_threads(
            parser, "count", "Number of enclave worker ECALL threads", {"enclave-worker-threads"}, 0);

        auto cli = add_default_network_args(argc, argv);
        try
        {
            parser.ParseCLI(cli.argc, cli.argv.data());
        }
        catch (const args::Help&)
        {
            std::cout << parser;
            std::exit(0);
        }
        catch (const args::ParseError& e)
        {
            std::ostringstream message;
            message << e.what() << "\n" << parser;
            throw std::invalid_argument(message.str());
        }

        auto cfg = net.get_config();
        cfg.log_values();

        canopy::network_config::tcp_endpoint listen_ep;
        if (const auto* endpoint = cfg.first_listen())
            listen_ep = *endpoint;
        else
            listen_ep.port = 8080;

        command_line output;
        output.port = listen_ep.port;
        output.listen_address = listen_ep.to_string();
        output.certificate_path = args::get(cert_file);
        output.private_key_path = args::get(key_file);
        output.static_root_path = args::get(static_root);
        output.enclave_worker_threads = args::get(enclave_worker_threads);

        if (output.static_root_path.empty())
            throw std::invalid_argument("--static-root must not be empty");

        const size_t listen_address_byte_count
            = listen_ep.family == canopy::network_config::ip_address_family::ipv6 ? listen_ep.addr.size() : 4;

        output.enclave_options.emplace("listen-address", output.listen_address);
        output.enclave_options.emplace(
            "listen-family",
            listen_ep.family == canopy::network_config::ip_address_family::ipv6 ? "ipv6" : "ipv4");
        output.enclave_options.emplace(
            "listen-address-bytes",
            std::string(reinterpret_cast<const char*>(listen_ep.addr.data()), listen_address_byte_count));
        output.enclave_options.emplace("listen-port", std::to_string(output.port));
        output.enclave_options.emplace("static-root", output.static_root_path);
        output.enclave_options.emplace("cert", output.certificate_path);
        output.enclave_options.emplace("key", output.private_key_path);
        return output;
    }

    auto make_scheduler() -> std::shared_ptr<coro::scheduler>
    {
        return std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
            coro::scheduler::options{
                .thread_strategy = coro::scheduler::thread_strategy_t::spawn,
                .pool = coro::thread_pool::options{
                    .thread_count = 1,
                },
                .execution_strategy = coro::scheduler::execution_strategy_t::process_tasks_on_thread_pool,
            }));
    }
} // namespace

auto main(
    int argc,
    char* argv[]) -> int
{
    command_line cli;
    try
    {
        cli = parse_command_line(argc, argv);
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }

    if ((!cli.certificate_path.empty() && cli.private_key_path.empty())
        || (cli.certificate_path.empty() && !cli.private_key_path.empty()))
    {
        RPC_ERROR("Both --cert and --key must be provided for TLS");
        return 1;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);

    auto scheduler = make_scheduler();
    auto root_service = rpc::root_service::create("websocket enclave host", rpc::DEFAULT_PREFIX, scheduler);

    rpc::io_uring::host_controller::options controller_options;
    controller_options.register_fixed_files = true;
    controller_options.fixed_file_count = 256;
    controller_options.sq_thread_idle_ms = 50;

    auto enclave_transport = std::make_shared<rpc::sgx::coro::host::transport>(
        "websocket enclave transport", root_service, CANOPY_WEBSOCKET_DEMO_ENCLAVE_PATH);
    enclave_transport->set_enclave_worker_thread_count(cli.enclave_worker_threads);
    auto startup_options_error = enclave_transport->set_enclave_startup_options(cli.enclave_options);
    if (startup_options_error != rpc::error::OK())
    {
        RPC_ERROR("invalid enclave startup options: {}", rpc::error::to_string(startup_options_error));
        scheduler->shutdown();
        return 1;
    }

    auto connect_result = coro::sync_wait(
        (rpc::sgx::coro::host::connect_to_enclave_zone<rpc::i_noop, websocket_demo::v1::i_enclave_websocket_server>(
            root_service, "websocket demo enclave", enclave_transport, {}, controller_options)));
    if (connect_result.error_code != rpc::error::OK() || !connect_result.output_interface)
    {
        RPC_ERROR("failed to connect to websocket enclave: {}", rpc::error::to_string(connect_result.error_code));
        scheduler->shutdown();
        return 1;
    }

    auto enclave_server = connect_result.output_interface;
    auto listen_error = coro::sync_wait(enclave_server->listen());
    if (listen_error != rpc::error::OK())
    {
        RPC_ERROR("failed to start enclave websocket listener: {}", rpc::error::to_string(listen_error));
        enclave_server = nullptr;
        scheduler->shutdown();
        return 1;
    }

    RPC_INFO("websocket enclave server listening on configured endpoint {} via enclave port {}", cli.listen_address, cli.port);
    while (g_stop_requested == 0)
        std::this_thread::sleep_for(std::chrono::milliseconds{200});

    RPC_INFO("websocket enclave server stopping");
    const auto stop_error = coro::sync_wait(enclave_server->stop());
    if (stop_error != rpc::error::OK())
        RPC_WARNING("websocket enclave server stop returned {}", rpc::error::to_string(stop_error));

    // The SGX/io_uring listener teardown can still block in process shutdown
    // while the listener path is being stabilised. This demo has already asked
    // the enclave server to stop; quick_exit lets the process terminate without
    // making this host executable the production teardown pattern.
    std::quick_exit(0);
}
