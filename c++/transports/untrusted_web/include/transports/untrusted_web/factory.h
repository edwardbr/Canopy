/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <functional>
#include <memory>
#include <utility>

#include <rpc/rpc.h>
#include <streaming/stream.h>
#include <transports/untrusted_web/transport.h>

namespace rpc::untrusted_web
{
    struct accept_result
    {
        int error_code{rpc::error::OK()};
        std::shared_ptr<rpc::service> service;
        std::shared_ptr<transport> transport;
    };

    CORO_TASK(accept_result)
    accept_transport(
        std::shared_ptr<streaming::stream> stream,
        transport::connection_handler handler,
        const transport_settings& settings = {},
        std::shared_ptr<rpc::service> service = {});

    template<
        class Remote,
        class Local>
    CORO_TASK(accept_result)
    accept_rpc(
        std::shared_ptr<streaming::stream> stream,
        std::function<CORO_TASK(rpc::service_connect_result<Local>)(
            const rpc::shared_ptr<Remote>&,
            const std::shared_ptr<rpc::service>&)> factory,
        const transport_settings& settings = {},
        std::shared_ptr<rpc::service> service = {})
    {
        if (!factory)
            CO_RETURN accept_result{rpc::error::INVALID_DATA(), {}, {}};

        auto handler = rpc::make_new_zone_connection_handler<Remote, Local>("untrusted_web", std::move(factory));
        CO_RETURN CO_AWAIT accept_transport(std::move(stream), std::move(handler), settings, std::move(service));
    }
} // namespace rpc::untrusted_web
