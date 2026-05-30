/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <chrono>
#include <cstdint>

#include <io_uring/host_io_uring.h>
#include <connection_factory/tcp_coroutine_types.h>
#include <connection_factory/options.h>
#include <streaming/tcp_coroutine/stream.h>
#include <tcp_coroutine_stream/tcp_coroutine_stream_config.h>

namespace rpc::tcp_coroutine
{
    // This header is the JSON/configuration boundary for TCP coroutine factories.
    // Core TCP coroutine stream mechanics and io_uring controllers consume generated typed
    // option structures; schema defaults and caller overlays are normalised here
    // before those mechanics are invoked.

    const json::v1::object& tcp_coroutine_default_options();

    struct materialise_tcp_coroutine_options_result
    {
        int error_code{rpc::error::OK()};
        ::rpc::tcp_coroutine_stream::endpoint options;
    };

    const json::v1::object& tcp_coroutine_options_schema();

    materialise_tcp_coroutine_options_result materialise_tcp_coroutine_options(const json::v1::object& client_options);

    struct tcp_coroutine_runtime_options_result
    {
        int error_code{rpc::error::OK()};
        rpc::connection_factory::stream_rpc_connection_settings factory_settings;
        rpc::io_uring::host_controller::options controller_options;
        ::streaming::coroutine::tcp::stream::options stream_options;
        uint16_t port{0};
        std::chrono::milliseconds connect_timeout{5000};
    };

    int validate_connect_endpoint(const ::rpc::tcp_coroutine_stream::endpoint& options) noexcept;

    namespace detail
    {
        tcp_coroutine_runtime_options_result make_tcp_coroutine_runtime_options(
            ::rpc::tcp_coroutine_stream::endpoint tcp_coroutine_options,
            rpc::connection_factory::stream_rpc_connection_settings factory_settings
            = rpc::connection_factory::make_stream_rpc_settings());
    } // namespace detail

} // namespace rpc::tcp_coroutine
