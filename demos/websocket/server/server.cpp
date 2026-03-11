// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#include "server.h"

#include "http_client_connection.h"

#include <rpc/rpc.h>
#include <streaming/tcp_stream.h>

namespace
{
    auto handle_client(coro::net::tcp::client client, std::shared_ptr<websocket_demo::v1::websocket_service> service)
        -> coro::task<void>
    {
        auto stream = std::make_shared<streaming::tcp_stream>(std::move(client), service->get_scheduler());
        websocket_demo::v1::http_client_connection connection(stream, service);
        co_await connection.handle();
        co_return;
    }

    auto handle_tls_client(coro::net::tcp::client client,
        std::shared_ptr<streaming::tls_context> tls_ctx,
        std::shared_ptr<websocket_demo::v1::websocket_service> service) -> coro::task<void>
    {
        auto tcp = std::make_shared<streaming::tcp_stream>(std::move(client), service->get_scheduler());
        auto stream = std::make_shared<streaming::tls_stream>(tcp, tls_ctx);

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
}

auto run_websocket_server(std::shared_ptr<coro::scheduler> scheduler,
    std::shared_ptr<websocket_demo::v1::websocket_service> service,
    std::shared_ptr<streaming::tls_context> tls_ctx,
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
}
