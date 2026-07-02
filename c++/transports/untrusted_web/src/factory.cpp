/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <transports/untrusted_web/factory.h>

#include <transports/factory.h>

namespace rpc::untrusted_web
{
    CORO_TASK(accept_result)
    accept_transport(
        std::shared_ptr<streaming::stream> stream,
        transport::connection_handler handler,
        transport_settings settings,
        std::shared_ptr<rpc::service> service)
    {
        if (!stream || !handler)
            CO_RETURN accept_result{rpc::error::INVALID_DATA(), {}, {}};

        auto resolved_service = rpc::transport_creation::ensure_service(
            std::move(service), rpc::optional<rpc::encoding>{}, "untrusted_web_accept");
        if (!resolved_service)
            CO_RETURN accept_result{rpc::error::INVALID_DATA(), {}, {}};

        auto accepted = CO_AWAIT transport::create(resolved_service, stream, std::move(handler), settings);
        if (!accepted)
            CO_RETURN accept_result{rpc::error::TRANSPORT_ERROR(), {}, {}};

        CO_RETURN accept_result{rpc::error::OK(), std::move(resolved_service), std::move(accepted)};
    }
} // namespace rpc::untrusted_web
