// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#include <canopy/http_server/http_acceptor.h>

#include <array>
#include <chrono>
#include <cstring>
#include <optional>

#ifdef CANOPY_BUILD_COROUTINE
#  include <arpa/inet.h>
#  include <coro/coro.hpp>
#  include <io_uring/host_io_uring.h>
#  include <streaming/tcp_coroutine/acceptor.h>
#  include <streaming/tcp_coroutine/stream.h>
#else
#  include <streaming/tcp_blocking/acceptor.h>
#  include <streaming/tcp_blocking/stream.h>
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
        // calls park on poll() inside the underlying streaming::blocking::tcp::stream.
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

#ifdef CANOPY_BUILD_COROUTINE
        auto parse_ipv4_address(const std::string& host) -> std::optional<std::array<
            uint8_t,
            4>>
        {
            std::array<uint8_t, 4> result{};
            if (host.empty())
                return result;

            in_addr parsed{};
            if (::inet_pton(AF_INET, host.c_str(), &parsed) != 1)
                return std::nullopt;

            static_assert(sizeof(result) == sizeof(parsed.s_addr));
            std::memcpy(result.data(), &parsed.s_addr, result.size());
            return result;
        }

        auto parse_ipv6_address(const std::string& host) -> std::optional<std::array<
            uint8_t,
            16>>
        {
            std::array<uint8_t, 16> result{};
            if (host.empty())
                return result;

            in6_addr parsed{};
            if (::inet_pton(AF_INET6, host.c_str(), &parsed) != 1)
                return std::nullopt;

            static_assert(sizeof(result) == sizeof(parsed.s6_addr));
            std::memcpy(result.data(), parsed.s6_addr, result.size());
            return result;
        }
#endif
    } // namespace

    auto run_server(
        endpoint ep,
        std::shared_ptr<rpc::executor> executor,
        accepted_stream_handler stream_handler,
        std::shared_ptr<streaming::secure::context> tls_context,
        stop_requested should_stop) -> CORO_TASK(void)
    {
#ifdef CANOPY_BUILD_COROUTINE
        if (!executor)
        {
            RPC_ERROR("http_server: TCP coroutine acceptor init failed (no executor)");
            CO_RETURN;
        }

        rpc::io_uring::linux_io_uring_handle::options handle_options;
        handle_options.queue_depth = 256;
        handle_options.buffer_count = 128;
        handle_options.buffer_size = 64U * 1024U;
        handle_options.fixed_file_count = 256;
        handle_options.register_fixed_files = true;

        std::shared_ptr<rpc::io_uring::linux_io_uring_handle> handle;
        auto io_error = rpc::io_uring::linux_io_uring_handle::create(handle, handle_options, executor);
        if (io_error != rpc::error::OK())
        {
            RPC_ERROR("http_server: failed to create TCP coroutine io_uring handle error={}", io_error);
            CO_RETURN;
        }

        auto controller = std::make_shared<rpc::io_uring::controller>(
            std::move(handle), executor.get(), rpc::io_uring::default_controller_options());
        auto acceptor = std::make_shared<streaming::coroutine::tcp::acceptor>(std::move(controller));

        int listen_error = rpc::error::INVALID_DATA();
        if (ep.ipv6)
        {
            auto address = parse_ipv6_address(ep.host);
            if (address)
                listen_error = CO_AWAIT acceptor->listen_ipv6(*address, ep.port);
        }
        else
        {
            auto address = parse_ipv4_address(ep.host);
            if (address)
                listen_error = CO_AWAIT acceptor->listen_ipv4(*address, ep.port);
        }

        if (listen_error != rpc::error::OK())
        {
            RPC_ERROR("http_server: TCP coroutine listen failed host={} port={} error={}", ep.host, ep.port, listen_error);
            CO_RETURN;
        }
#else
        streaming::blocking::tcp::endpoint tcp_ep;
        tcp_ep.host = ep.host;
        tcp_ep.port = ep.port;
        tcp_ep.ipv6 = ep.ipv6;
        auto acceptor = std::make_shared<streaming::blocking::tcp::acceptor>(tcp_ep);
#endif
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
