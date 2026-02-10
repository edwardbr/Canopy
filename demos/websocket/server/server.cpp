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
    auto stream = std::make_shared<websocket_demo::v1::tcp_stream>(std::move(client));
    websocket_demo::v1::http_client_connection connection(stream, service);
    co_await connection.handle();
    co_return;
}

// Handle a TLS client connection
auto handle_tls_client(coro::net::tcp::client client,
    std::shared_ptr<websocket_demo::v1::tls_context> tls_ctx,
    std::shared_ptr<websocket_demo::v1::websocket_service> service) -> coro::task<void>
{
    auto stream = std::make_shared<websocket_demo::v1::tls_stream>(std::move(client), tls_ctx);

    // Perform TLS handshake
    bool handshake_ok = co_await stream->handshake();
    if (!handshake_ok)
    {
        std::cerr << "TLS handshake failed, closing connection" << std::endl;
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
            std::cerr << "Unknown argument: " << arg << std::endl;
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
            std::cerr << "Failed to initialize TLS context, exiting" << std::endl;
            return 1;
        }
        std::cout << "TLS enabled with certificate: " << cert_file << std::endl;
    }
    else if (!cert_file.empty() || !key_file.empty())
    {
        std::cerr << "Both --cert and --key must be provided for TLS" << std::endl;
        return 1;
    }

    // Ignore SIGPIPE to prevent crashes when writing to closed sockets.
    // When a client disconnects abruptly (e.g., browser refresh), sending data
    // to the closed socket would normally generate SIGPIPE and terminate the process.
    std::signal(SIGPIPE, SIG_IGN);

    auto scheduler = coro::io_scheduler::make_shared(
        coro::io_scheduler::options{
            // The scheduler will spawn a dedicated event processing thread.  This is the default, but
            // it is possible to use 'manual' and call 'process_events()' to drive the scheduler yourself.
            .thread_strategy = coro::io_scheduler::thread_strategy_t::spawn,
            // If the scheduler is in spawn mode this functor is called upon starting the dedicated
            // event processor thread.
            .on_io_thread_start_functor = [] { std::cout << "io_scheduler::process event thread start\n"; },
            // If the scheduler is in spawn mode this functor is called upon stopping the dedicated
            // event process thread.
            .on_io_thread_stop_functor = [] { std::cout << "io_scheduler::process event thread stop\n"; },
            // The io scheduler can use a coro::thread_pool to process the events or tasks it is given.
            // You can use an execution strategy of `process_tasks_inline` to have the event loop thread
            // directly process the tasks, this might be desirable for small tasks vs a thread pool for large tasks.
            .pool =
                coro::thread_pool::options{
                    .thread_count            = std::thread::hardware_concurrency(),
                    .on_thread_start_functor = [](size_t i)
                    { std::cout << "io_scheduler::thread_pool worker " << i << " starting\n"; },
                    .on_thread_stop_functor = [](size_t i)
                    { std::cout << "io_scheduler::thread_pool worker " << i << " stopping\n"; },
                },
            .execution_strategy = coro::io_scheduler::execution_strategy_t::process_tasks_on_thread_pool});

    std::atomic<uint64_t> zone_gen = 0;
    auto root_service = std::make_shared<websocket_demo::v1::websocket_service>("demo", rpc::zone{++zone_gen}, scheduler);
    root_service->generate_new_zone_id();
    // auto demo = websocket_demo::v1::create_websocket_demo_instance();

    auto make_websocket_server = [](std::shared_ptr<coro::io_scheduler> scheduler,
                                     std::shared_ptr<websocket_demo::v1::websocket_service> service,
                                     std::shared_ptr<websocket_demo::v1::tls_context> tls_ctx,
                                     uint16_t port) -> coro::task<void>
    {
        co_await scheduler->schedule();
        coro::net::tcp::server server{scheduler, coro::net::tcp::server::options{.port = port}};

        if (tls_ctx)
        {
            std::cout << "WebSocket server listening on port " << port << " (TLS enabled)" << std::endl;
        }
        else
        {
            std::cout << "WebSocket server listening on port " << port << std::endl;
        }

        while (true)
        {
            // Wait for a new connection
            auto pstatus = co_await server.poll();
            switch (pstatus)
            {
            case coro::poll_status::event:
            {
                auto client = server.accept();
                if (client.socket().is_valid())
                {
                    std::cout << "New client connected" << std::endl;
                    if (tls_ctx)
                    {
                        scheduler->spawn(handle_tls_client(std::move(client), tls_ctx, service));
                    }
                    else
                    {
                        scheduler->spawn(handle_client(std::move(client), service));
                    }
                }
            }
            break;
            case coro::poll_status::error:
            case coro::poll_status::closed:
            case coro::poll_status::timeout:
            default:
                std::cerr << "Server poll error, exiting" << std::endl;
                co_return;
            }
        }

        co_return;
    };

    coro::sync_wait(coro::when_all(make_websocket_server(scheduler, root_service, tls_ctx, port)));
}
