// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <rpc/rpc.h>
#include <streaming/secure_stream.h>
#include <streaming/stream.h>
#include <transports/streaming/transport.h>

namespace canopy::http_server
{
    // Vendor-neutral bind endpoint. Replaces direct use of
    // coro::net::ip_address / coro::net::socket_address at the API surface.
    struct endpoint
    {
        std::string host = "0.0.0.0";
        uint16_t port = 0;
        bool ipv6 = false;
    };

    using accepted_stream_handler
        = std::function<CORO_TASK(std::shared_ptr<rpc::transport>)(std::shared_ptr<streaming::stream>)>;
    using stop_requested = std::function<bool()>;

    // run_server drives the HTTP accept loop. In coroutine builds it is a
    // CORO_TASK(void) you co_await on the scheduler. In blocking builds it
    // posts the accept loop to the executor and returns once the listener is
    // up; should_stop drives the exit.
    auto run_server(
        endpoint ep,
        std::shared_ptr<rpc::executor> executor,
        accepted_stream_handler stream_handler,
        std::shared_ptr<streaming::secure::context> tls_context,
        stop_requested should_stop = {}) -> CORO_TASK(void);
} // namespace canopy::http_server
