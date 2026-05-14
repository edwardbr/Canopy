// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#include <csignal>
#include <cstdlib>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>

#include <coro/coro.hpp>
#include <io_uring/host_controller.h>
#include <rpc/rpc.h>
#include <transports/sgx_coroutine/host/connect.h>
#include <transports/sgx_coroutine/host/transport.h>
#include <websocket_demo/websocket_demo.h>

#ifndef CANOPY_WEBSOCKET_DEMO_ENCLAVE_PATH
#  error "CANOPY_WEBSOCKET_DEMO_ENCLAVE_PATH must be defined for websocket_enclave_server"
#endif

namespace
{
    volatile std::sig_atomic_t g_stop_requested = 0;

    struct command_line
    {
        uint16_t port{8080};
        std::string certificate_path;
        std::string private_key_path;
        uint32_t enclave_worker_threads{0};
    };

    void on_signal(int signal_number)
    {
        // The repeated-signal path is inside an async signal handler; use the
        // signal-safe hard exit rather than running C++ cleanup machinery here.
        if (g_stop_requested != 0)
            std::_Exit(128 + signal_number);
        g_stop_requested = 1;
    }

    auto default_cert_path(const char* source_file) -> std::string
    {
        return (std::filesystem::path(source_file).parent_path() / "certs" / "server.crt").string();
    }

    auto default_key_path(const char* source_file) -> std::string
    {
        return (std::filesystem::path(source_file).parent_path() / "certs" / "server.key").string();
    }

    auto starts_with(
        std::string_view text,
        std::string_view prefix) -> bool
    {
        return text.rfind(prefix, 0) == 0;
    }

    auto parse_port(std::string_view value) -> uint16_t
    {
        const auto parsed = std::stoul(std::string(value));
        if (parsed == 0 || parsed > UINT16_MAX)
            throw std::invalid_argument("port must be in the range 1..65535");
        return static_cast<uint16_t>(parsed);
    }

    auto parse_worker_thread_count(std::string_view value) -> uint32_t
    {
        const auto parsed = std::stoul(std::string(value));
        if (parsed > UINT32_MAX)
            throw std::invalid_argument("worker thread count must fit in uint32_t");
        return static_cast<uint32_t>(parsed);
    }

    auto parse_command_line(
        int argc,
        char* argv[]) -> command_line
    {
        command_line output;
        output.certificate_path = default_cert_path(__FILE__);
        output.private_key_path = default_key_path(__FILE__);

        for (int index = 1; index < argc; ++index)
        {
            const std::string_view arg = argv[index];
            if (arg == "--help" || arg == "-h")
            {
                std::cout << "Usage: websocket_enclave_server [--port=N] [--cert=FILE] [--key=FILE] "
                             "[--enclave-worker-threads=N]\n";
                std::exit(0);
            }
            if (starts_with(arg, "--port="))
            {
                output.port = parse_port(arg.substr(7));
                continue;
            }
            if (starts_with(arg, "--cert="))
            {
                output.certificate_path = std::string(arg.substr(7));
                continue;
            }
            if (starts_with(arg, "--key="))
            {
                output.private_key_path = std::string(arg.substr(6));
                continue;
            }
            if (starts_with(arg, "--enclave-worker-threads="))
            {
                output.enclave_worker_threads = parse_worker_thread_count(arg.substr(25));
                continue;
            }

            throw std::invalid_argument("unknown argument: " + std::string(arg));
        }

        return output;
    }

    auto read_text_file(const std::string& path) -> std::string
    {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
            throw std::runtime_error("unable to open " + path);

        std::ostringstream buffer;
        buffer << file.rdbuf();
        auto content = buffer.str();
        if (content.empty())
            throw std::runtime_error("empty file " + path);
        return content;
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

    std::string certificate_pem;
    std::string private_key_pem;
    try
    {
        certificate_pem = read_text_file(cli.certificate_path);
        private_key_pem = read_text_file(cli.private_key_path);
    }
    catch (const std::exception& e)
    {
        RPC_ERROR("{}", e.what());
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
    auto listen_error = coro::sync_wait(enclave_server->listen(cli.port, certificate_pem, private_key_pem));
    if (listen_error != rpc::error::OK())
    {
        RPC_ERROR("failed to start enclave websocket listener: {}", rpc::error::to_string(listen_error));
        enclave_server = nullptr;
        scheduler->shutdown();
        return 1;
    }

    RPC_INFO("websocket enclave server listening on https://127.0.0.1:{}", cli.port);
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
