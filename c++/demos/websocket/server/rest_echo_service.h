// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#pragma once

#include <rpc/rpc.h>
#include <websocket_rest/websocket_rest.h>

namespace websocket_demo
{
    namespace v1
    {
        auto make_echo_service() -> rpc::shared_ptr<websocket_demo::rest::v1::i_echo>;
    }
}
