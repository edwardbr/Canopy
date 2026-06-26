// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#pragma once

#include <functional>
#include <memory>

#include <canopy/http_server/http_client_connection.h>
#include <rpc/rpc.h>
#include <websocket_demo/websocket_demo.h>

namespace websocket_demo
{
    namespace v1
    {
        auto make_websocket_upgrade_handler(std::shared_ptr<rpc::service> service)
            -> canopy::http_server::websocket_handler;
    }
}
