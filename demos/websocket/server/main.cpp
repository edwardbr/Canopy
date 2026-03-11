// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#include "server.h"

#include <csignal>
#include <iostream>
#include <memory>
#include <string>

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
    void print_usage(const char* program)
    {
        std::cout << "Usage: " << program << " [options]\n"
                  << "Options:\n"
                  << "  --cert <file>   Path to TLS certificate file (PEM format)\n"
                  << "  --key <file>    Path to TLS private key file (PEM format)\n"
                  << "  --port <port>   Port to listen on (default: 8888)\n"
                  << "  --help          Show this help message\n"
                  << "\n"
                  << "If both --cert and --key are provided, the server will use TLS (HTTPS/WSS).\n"
                  << "Otherwise, it will use plain HTTP/WS.\n"
                  << "\n"
                  << "Usage\n"
                  << "\n"
                  << "Plain HTTP/WS (no change):\n"
                  << "./websocket_server\n"
                  << "\n"
                  << "With TLS (HTTPS/WSS):\n"
                  << "./websocket_server --cert server.crt --key server.key --port 8443\n"
                  << "\n"
                  << "To generate a self-signed certificate for testing:\n"
                  << "openssl req -x509 -newkey rsa:4096 -keyout server.key -out server.crt -days "
                     "365 -nodes -subj \"/CN=localhost\"\n";
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

auto main(int argc, char* argv[]) -> int
{
    std::string cert_file;
    std::string key_file;
    uint16_t port = 8888;

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "--cert" && i + 1 < argc)
        {
            cert_file = argv[++i];
        }
        else if (arg == "--key" && i + 1 < argc)
        {
            key_file = argv[++i];
        }
        else if (arg == "--port" && i + 1 < argc)
        {
            port = static_cast<uint16_t>(std::stoi(argv[++i]));
        }
        else if (arg == "--help")
        {
            print_usage(argv[0]);
            return 0;
        }
        else
        {
            RPC_ERROR("Unknown argument: {}", arg);
            print_usage(argv[0]);
            return 1;
        }
    }

    std::shared_ptr<streaming::tls_context> tls_ctx;
    if (!cert_file.empty() && !key_file.empty())
    {
        tls_ctx = std::make_shared<streaming::tls_context>(cert_file, key_file);
        if (!tls_ctx->is_valid())
        {
            RPC_ERROR("Failed to initialize TLS context, exiting");
            return 1;
        }
        RPC_INFO("TLS enabled with certificate: {}", cert_file);
    }
    else if (!cert_file.empty() || !key_file.empty())
    {
        RPC_ERROR("Both --cert and --key must be provided for TLS");
        return 1;
    }

    std::signal(SIGPIPE, SIG_IGN);

    auto scheduler = make_scheduler();
    auto root_service = std::make_shared<websocket_demo::v1::websocket_service>("demo", rpc::DEFAULT_PREFIX, scheduler);

    coro::sync_wait(coro::when_all(run_websocket_server(scheduler, root_service, tls_ctx, port)));
}
