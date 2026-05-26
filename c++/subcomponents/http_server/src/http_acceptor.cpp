// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#include <canopy/http_server/http_acceptor.h>

#include <chrono>

#include <streaming/tcp/acceptor.h>
#include <streaming/tcp/stream.h>

#ifdef CANOPY_BUILD_COROUTINE
#  include <coro/coro.hpp>
#endif

namespace canopy::http_server
{
    namespace
    {
        // Plain (non-TLS) per-client handler — identical in both modes.
        auto handle_plain_client(
            std::shared_ptr<streaming::stream> stream,
            accepted_stream_handler stream_handler) -> CORO_TASK(void)
        {
            auto handler = CO_AWAIT stream_handler(std::move(stream));
            if (handler)
            {
                CO_AWAIT handler->inner_accept();
            }
            CO_RETURN;
        }

        // TLS wrapping over the dual-mode streaming::secure::stream. Works in
        // both modes — coroutine yields on the underlying I/O; blocking
        // calls park on poll() inside the underlying streaming::tcp::stream.
        auto handle_tls_client(
            std::shared_ptr<streaming::stream> tcp_stream,
            std::shared_ptr<streaming::secure::context> tls_context,
            accepted_stream_handler stream_handler) -> CORO_TASK(void)
        {
            auto tls_stream = std::make_shared<streaming::secure::stream>(tcp_stream, tls_context);

            bool handshake_ok = CO_AWAIT tls_stream->handshake();
            if (!handshake_ok)
            {
                RPC_ERROR("TLS handshake failed, closing connection");
                CO_RETURN;
            }

            auto handler = CO_AWAIT stream_handler(std::move(tls_stream));
            if (handler)
            {
                CO_AWAIT handler->inner_accept();
            }
            CO_RETURN;
        }
    } // namespace

    auto run_server(
        endpoint ep,
        std::shared_ptr<rpc::executor> executor,
        accepted_stream_handler stream_handler,
        std::shared_ptr<streaming::secure::context> tls_context,
        stop_requested should_stop) -> CORO_TASK(void)
    {
        streaming::tcp::endpoint tcp_ep;
        tcp_ep.host = ep.host;
        tcp_ep.port = ep.port;
        tcp_ep.ipv6 = ep.ipv6;
        auto acceptor = std::make_shared<streaming::tcp::acceptor>(tcp_ep);
        if (!acceptor->init(executor))
        {
            RPC_ERROR("http_server: TCP acceptor init failed (no executor or bind error)");
            CO_RETURN;
        }

        if (tls_context)
        {
            RPC_INFO("WebSocket server listening on port {} (TLS enabled)", ep.port);
        }
        else
        {
            RPC_INFO("WebSocket server listening on port {}", ep.port);
        }

        while (!should_stop || !should_stop())
        {
            auto maybe_stream = CO_AWAIT acceptor->accept();
            if (should_stop && should_stop())
                break;

            if (!maybe_stream)
            {
                if (should_stop && should_stop())
                    break;
                // No stream and not stopping — the acceptor returned
                // nullopt because it was stopped externally or hit a fatal
                // error; exit the loop in either case.
                break;
            }

            RPC_INFO("New client connected");
            auto stream = std::move(*maybe_stream);
            bool spawned = false;
            if (tls_context)
            {
                auto stream_for_task = stream;
                spawned = executor->SPAWN_DETACHED(
                    handle_tls_client(std::move(stream_for_task), tls_context, stream_handler));
            }
            else
            {
                auto stream_for_task = stream;
                spawned = executor->SPAWN_DETACHED(handle_plain_client(std::move(stream_for_task), stream_handler));
            }
            if (!spawned)
            {
                RPC_ERROR("http_server: failed to spawn client handler");
                CO_AWAIT stream->set_closed();
            }
        }

        acceptor->stop();
        RPC_INFO("WebSocket server stopping");
        CO_RETURN;
    }
} // namespace canopy::http_server
