// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#include <iostream>
#include <atomic>
#include <memory>
#include <csignal>
#include <string>

#include <coro/coro.hpp>
#include <rpc/rpc.h>
#include "websocket_service.h"
#include "http_client_connection.h"
#include "tcp_stream.h"
#include "tls_stream.h"
#include "demo.h"

// Handle a plain TCP client connection
auto handle_client(coro::net::tcp::client client, std::shared_ptr<websocket_demo::v1::websocket_service> service)
    -> coro::task<void>
{
    auto stream = std::make_shared<websocket_demo::v1::tcp_stream>(std::move(client), service->get_scheduler());
    websocket_demo::v1::http_client_connection connection(stream, service);
    co_await connection.handle();
    co_return;
}

// Handle a TLS client connection
auto handle_tls_client(coro::net::tcp::client client,
    std::shared_ptr<websocket_demo::v1::tls_context> tls_ctx,
    std::shared_ptr<websocket_demo::v1::websocket_service> service) -> coro::task<void>
{
    auto tcp = std::make_shared<websocket_demo::v1::tcp_stream>(std::move(client), service->get_scheduler());
    auto stream = std::make_shared<websocket_demo::v1::tls_stream>(tcp, tls_ctx);

    // Perform TLS handshake
    bool handshake_ok = co_await stream->handshake();
    if (!handshake_ok)
    {
        RPC_ERROR("TLS handshake failed, closing connection");
        co_return;
    }

    websocket_demo::v1::http_client_connection connection(stream, service);
    co_await connection.handle();
    co_return;
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

auto main(int argc, char* argv[]) -> int
{
    // Parse command line arguments
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

    // Create TLS context if certificates are provided
    std::shared_ptr<websocket_demo::v1::tls_context> tls_ctx;

    if (!cert_file.empty() && !key_file.empty())
    {
        tls_ctx = std::make_shared<websocket_demo::v1::tls_context>(cert_file, key_file);
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

    // Ignore SIGPIPE to prevent crashes when writing to closed sockets.
    // When a client disconnects abruptly (e.g., browser refresh), sending data
    // to the closed socket would normally generate SIGPIPE and terminate the process.
    std::signal(SIGPIPE, SIG_IGN);

    auto scheduler = std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
        coro::scheduler::options{
            // The scheduler will spawn a dedicated event processing thread.  This is the default, but
            // it is possible to use 'manual' and call 'process_events()' to drive the scheduler yourself.
            .thread_strategy = coro::scheduler::thread_strategy_t::spawn,
            // If the scheduler is in spawn mode this functor is called upon starting the dedicated
            // event processor thread.
            .on_io_thread_start_functor = [] {
        RPC_DEBUG("process event thread start");},
            // If the scheduler is in spawn mode this functor is called upon stopping the dedicated
            // event process thread.
            .on_io_thread_stop_functor = []{
        RPC_DEBUG("io_scheduler::process event thread stop");},
            // The io scheduler can use a coro::thread_pool to process the events or tasks it is given.
            // You can use an execution strategy of `process_tasks_inline` to have the event loop thread
            // directly process the tasks, this might be desirable for small tasks vs a thread pool for large tasks.
            .pool =
                coro::thread_pool::options{
                    .thread_count            = std::thread::hardware_concurrency(),
                    .on_thread_start_functor = [](size_t i)
                    {
        RPC_DEBUG("io_scheduler::thread_pool worker {} starting", i); },
                    .on_thread_stop_functor = [](size_t i)
                    {
        RPC_DEBUG("io_scheduler::thread_pool worker {} stopping", i); },
                },
            .execution_strategy = coro::scheduler::execution_strategy_t::process_tasks_on_thread_pool}));

    auto root_service = std::make_shared<websocket_demo::v1::websocket_service>("demo", rpc::DEFAULT_PREFIX, scheduler);
    // auto demo = websocket_demo::v1::create_websocket_demo_instance();

    auto make_websocket_server = [](std::shared_ptr<coro::scheduler> scheduler,
                                     std::shared_ptr<websocket_demo::v1::websocket_service> service,
                                     std::shared_ptr<websocket_demo::v1::tls_context> tls_ctx,
                                     uint16_t port) -> coro::task<void>
    {
        co_await scheduler->schedule();
        coro::net::tcp::server server{scheduler, coro::net::socket_address{"0.0.0.0", port}};

        if (tls_ctx)
        {
            RPC_INFO("WebSocket server listening on port {} (TLS enabled)", port);
        }
        else
        {
            RPC_INFO("WebSocket server listening on port {}", port);
        }

        while (true)
        {
            // Wait for a new connection (zero timeout = wait indefinitely)
            auto client = co_await server.accept();
            if (client)
            {
                RPC_INFO("New client connected");
                if (tls_ctx)
                {
                    scheduler->spawn_detached(handle_tls_client(std::move(*client), tls_ctx, service));
                }
                else
                {
                    scheduler->spawn_detached(handle_client(std::move(*client), service));
                }
            }
            else if (!client.error().is_timeout())
            {
                RPC_ERROR("Server accept error, exiting");
                co_return;
            }
        }

        co_return;
    };

    coro::sync_wait(coro::when_all(make_websocket_server(scheduler, root_service, tls_ctx, port)));
}
