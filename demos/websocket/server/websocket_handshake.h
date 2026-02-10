// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

// websocket_handshake.h
#pragma once

#include <string_view>

namespace websocket_demo
{
    namespace v1
    {
        std::string calculate_ws_accept(std::string_view client_key);
    }
}
