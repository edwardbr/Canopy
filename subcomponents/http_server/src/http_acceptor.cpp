// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#include <canopy/http_server/http_acceptor.h>

#include <streaming/tcp/stream.h>

namespace canopy::http_server
{
    namespace
    {
        auto handle_client(
            coro::net::tcp::client client,
            std::shared_ptr<coro::scheduler> scheduler,
            accepted_stream_handler stream_handler) -> coro::task<void>
        {
            auto stream = std::make_shared<streaming::tcp::stream>(std::move(client), scheduler);
            auto handler = co_await stream_handler(std::move(stream));
            if (handler)
            {
                co_await handler->inner_accept();
            }
            co_return;
        }

        auto handle_tls_client(
            coro::net::tcp::client client,
            std::shared_ptr<coro::scheduler> scheduler,
            std::shared_ptr<streaming::tls::context> tls_context,
            accepted_stream_handler stream_handler) -> coro::task<void>
        {
            auto tcp_stream = std::make_shared<streaming::tcp::stream>(std::move(client), scheduler);
            auto tls_stream = std::make_shared<streaming::tls::stream>(tcp_stream, tls_context);

            bool handshake_ok = co_await tls_stream->handshake();
            if (!handshake_ok)
            {
                RPC_ERROR("TLS handshake failed, closing connection");
                co_return;
            }

            auto handler = co_await stream_handler(std::move(tls_stream));
            if (handler)
            {
                co_await handler->inner_accept();
            }
            co_return;
        }
    } // namespace

    auto run_server(
        coro::net::ip_address bind_address,
        uint16_t port,
        std::shared_ptr<coro::scheduler> scheduler,
        accepted_stream_handler stream_handler,
        std::shared_ptr<streaming::tls::context> tls_context) -> coro::task<void>
    {
        co_await scheduler->schedule();
        coro::net::tcp::server server{scheduler, coro::net::socket_address{bind_address, port}};

        if (tls_context)
        {
            RPC_INFO("WebSocket server listening on port {} (TLS enabled)", port);
        }
        else
        {
            RPC_INFO("WebSocket server listening on port {}", port);
        }

        while (true)
        {
            auto client = co_await server.accept();
            if (client)
            {
                RPC_INFO("New client connected");
                if (tls_context)
                {
                    scheduler->spawn_detached(
                        handle_tls_client(std::move(*client), scheduler, tls_context, stream_handler));
                }
                else
                {
                    scheduler->spawn_detached(handle_client(std::move(*client), scheduler, stream_handler));
                }
            }
            else if (!client.error().is_timeout())
            {
                RPC_ERROR("Server accept error, exiting");
                co_return;
            }
        }
    }
} // namespace canopy::http_server
