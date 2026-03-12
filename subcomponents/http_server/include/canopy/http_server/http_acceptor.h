// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include <coro/coro.hpp>
#include <rpc/rpc.h>
#include <streaming/stream.h>
#include <streaming/tls/stream.h>
#include <transports/streaming/transport.h>

namespace canopy::http_server
{
    using accepted_stream_handler
        = std::function<coro::task<std::shared_ptr<rpc::transport>>(std::shared_ptr<streaming::stream>)>;

    auto run_server(coro::net::ip_address bind_address,
        uint16_t port,
        std::shared_ptr<coro::scheduler> scheduler,
        accepted_stream_handler stream_handler,
        std::shared_ptr<streaming::tls::context> tls_context) -> coro::task<void>;
} // namespace canopy::http_server
