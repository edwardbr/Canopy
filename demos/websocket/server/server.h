// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#pragma once

#include <cstdint>
#include <memory>

#include <coro/coro.hpp>

#include "websocket_service.h"
#include <streaming/tls_stream.h>

auto run_websocket_server(std::shared_ptr<coro::scheduler> scheduler,
    std::shared_ptr<websocket_demo::v1::websocket_service> service,
    std::shared_ptr<streaming::tls_context> tls_ctx,
    uint16_t port) -> coro::task<void>;
