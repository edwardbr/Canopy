/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

/*
 *   echo_impl.h - Implementation of i_echo for the stream_composition demo
 */

#pragma once

#include <rpc/rpc.h>
#include <echo/echo.h>

namespace stream_composition
{
    class echo_impl : public rpc::base<echo_impl, i_echo>
    {
    public:
        CORO_TASK(int)
        echo(
            const std::string& message,
            std::string& response) override
        {
            response = "Echo: " + message;
            CO_RETURN rpc::error::OK();
        }
    };
} // namespace stream_composition
