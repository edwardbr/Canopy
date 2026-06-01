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
    int configure_service(
        const std::shared_ptr<rpc::service>& service,
        const rpc::stream_transport::transport_settings& settings);

    std::shared_ptr<rpc::service> ensure_service(
        const service_settings& settings,
        const rpc::stream_transport::transport_settings& transport_settings,
        std::shared_ptr<rpc::service> service,
        std::string default_name);

    std::shared_ptr<rpc::service> ensure_service(
        const stream_rpc_connection_settings& settings,
        std::shared_ptr<rpc::service> service,
        std::string default_name);

    std::shared_ptr<rpc::service> ensure_service(
        const rpc::stream_transport::transport_settings& settings,
        std::shared_ptr<rpc::service> service,
        std::string default_name);
} // namespace rpc::connection_factory
