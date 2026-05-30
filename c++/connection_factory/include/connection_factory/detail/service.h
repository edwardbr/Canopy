/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <memory>
#include <string>

#include <connection_factory/options.h>
#include <rpc/rpc.h>

namespace rpc::connection_factory
{
    inline int configure_service(
        const std::shared_ptr<rpc::service>& service,
        const rpc::stream_transport::transport_settings& settings)
    {
        return rpc::stream_transport::configure_service(service, settings);
    }

    inline std::shared_ptr<rpc::service> ensure_service(
        const service_settings& settings,
        const rpc::stream_transport::transport_settings& transport_settings,
        std::shared_ptr<rpc::service> service,
        std::string default_name)
    {
        rpc::stream_transport::service_settings stream_service_settings;
        stream_service_settings.name = settings.name;
        return rpc::stream_transport::ensure_service(
            stream_service_settings, transport_settings, std::move(service), std::move(default_name));
    }

    inline std::shared_ptr<rpc::service> ensure_service(
        const stream_rpc_connection_settings& settings,
        std::shared_ptr<rpc::service> service,
        std::string default_name)
    {
        return rpc::stream_transport::ensure_service(settings, std::move(service), std::move(default_name));
    }

    inline std::shared_ptr<rpc::service> ensure_service(
        const rpc::stream_transport::transport_settings& settings,
        std::shared_ptr<rpc::service> service,
        std::string default_name)
    {
        return rpc::stream_transport::ensure_service(settings, std::move(service), std::move(default_name));
    }
} // namespace rpc::connection_factory
