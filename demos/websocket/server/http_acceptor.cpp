// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#include "http_acceptor.h"

#include "http_client_connection.h"

#include <canopy/http_server/http_acceptor.h>

auto run_http_server(std::shared_ptr<coro::scheduler> scheduler,
    coro::net::ip_address bind_address,
    uint16_t port,
    std::shared_ptr<websocket_demo::v1::websocket_service> service,
    std::shared_ptr<streaming::tls_context> tls_ctx) -> coro::task<void>
{
    auto stream_handler
        = [service](
              std::shared_ptr<streaming::stream> stream) -> coro::task<std::shared_ptr<rpc::stream_transport::transport>>
    {
        websocket_demo::v1::http_client_connection connection(std::move(stream), service);
        co_return CO_AWAIT connection.handle();
    };

    co_return CO_AWAIT canopy::http_server::run_server(
        std::move(bind_address), port, scheduler, std::move(stream_handler), tls_ctx);
}
