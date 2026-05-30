/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <cstdint>

namespace rpc::tcp_coroutine
{
    struct loopback_port_range
    {
        uint16_t first_port{26000};
        uint16_t last_port{26064};
    };

    struct loopback_listen_options
    {
        // A non-zero port asks for one exact loopback port. Zero means scan the
        // range below and use the first available port.
        uint16_t port{0};
        loopback_port_range port_range{};
    };
} // namespace rpc::tcp_coroutine
