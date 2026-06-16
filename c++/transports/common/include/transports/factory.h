/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <rpc/rpc.h>

namespace rpc::transport_creation
{
    struct connect_result
    {
        int error_code{rpc::error::OK()};
        std::shared_ptr<rpc::service> service;
        std::shared_ptr<rpc::transport> transport;
        std::string service_proxy_name;
    };

    inline std::string configured_name(
        const rpc::optional<std::string>& configured,
        std::string fallback)
    {
        if (!configured)
            return fallback;
        return configured.value();
    }

    inline std::optional<rpc::encoding> encoding_option(const rpc::optional<rpc::encoding>& configured)
    {
        if (!configured)
            return std::nullopt;
        if (configured.value() == rpc::encoding::not_set)
            return std::nullopt;
        return configured.value();
    }

    inline int configure_service_encoding(
        const std::shared_ptr<rpc::service>& service,
        const rpc::optional<rpc::encoding>& configured)
    {
        if (!service)
            return rpc::error::OK();
        if (auto encoding = encoding_option(configured))
            service->set_default_encoding(*encoding);
        return rpc::error::OK();
    }

    inline std::shared_ptr<rpc::service> ensure_service(
        std::shared_ptr<rpc::service> service,
        const rpc::optional<rpc::encoding>& encoding,
        std::string default_name,
        rpc::executor_ptr executor = {})
    {
        if (service)
        {
            configure_service_encoding(service, encoding);
            return service;
        }

        if (!executor)
            executor = rpc::make_executor();

        rpc::service_config config;
        auto created = rpc::root_service::create(std::move(default_name), config, std::move(executor));
        configure_service_encoding(created, encoding);
        return created;
    }

    template<
        class In,
        class Out>
    CORO_TASK(rpc::service_connect_result<Out>)
    connect_rpc(
        rpc::shared_ptr<In> input_interface,
        connect_result connection)
    {
        if (connection.error_code != rpc::error::OK())
            CO_RETURN rpc::service_connect_result<Out>{connection.error_code, {}};
        if (!connection.service || !connection.transport)
            CO_RETURN rpc::service_connect_result<Out>{rpc::error::INVALID_DATA(), {}};

        CO_RETURN CO_AWAIT connection.service->template connect_to_zone<In, Out>(
            std::move(connection.service_proxy_name), std::move(connection.transport), std::move(input_interface));
    }
} // namespace rpc::transport_creation
