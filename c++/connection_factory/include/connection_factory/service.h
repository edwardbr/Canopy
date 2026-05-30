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
    // Service wiring is shared by stream-only and RPC factories. A service may
    // be supplied by the application when it already has a root service, or
    // created here when the caller only asked for a stream/RPC connection. JSON
    // configuration never carries service pointers; it only controls names and
    // serialisation defaults.

    // Factory helpers need an executor even when the caller is only creating a
    // stream and has not provided a service. In coroutine builds this creates a
    // scheduler; in blocking builds it creates the normal executor.
    rpc::executor_ptr make_default_executor();

    // Apply service-level options that can be changed on an existing service.
    // Ownership stays with the caller when a service is passed into a factory.
    int configure_service(
        const std::shared_ptr<rpc::service>& service,
        const rpc::stream_transport::transport_settings& settings);

    // Return the caller's service when provided, otherwise create a root service
    // using the configured or default name. The shared_ptr is deliberately a
    // std::shared_ptr because these helpers sit outside marshalled IDL ownership.
    std::shared_ptr<rpc::service> ensure_service(
        const rpc::connection_factory_config::service::settings& service_settings,
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
