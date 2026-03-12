// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#pragma once

#include <cstdint>
#include <memory>

#include <coro/coro.hpp>

#include "demo_zone.h"
#include <streaming/tls_stream.h>

auto run_http_server(std::shared_ptr<coro::scheduler> scheduler,
    coro::net::ip_address bind_address,
    uint16_t port,
    std::shared_ptr<websocket_demo::v1::websocket_service> service,
    std::shared_ptr<streaming::tls_context> tls_ctx) -> coro::task<void>;
