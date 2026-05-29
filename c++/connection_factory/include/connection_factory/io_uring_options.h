/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <cstdint>
#include <utility>

#include <io_uring/host_io_uring.h>
#include <connection_factory/io_uring_types.h>
#include <connection_factory/options.h>
#include <streaming/io_uring/stream.h>

namespace rpc::io_uring
{
    // This header is the JSON/configuration boundary for io_uring factories.
    // Core io_uring stream and controller mechanics consume generated typed
    // option structures; schema defaults and caller overlays are normalised here
    // before those mechanics are invoked.

    inline const json::v1::object& io_uring_default_options()
    {
        static const json::v1::object options = []
        {
            rpc::connection_factory_config::stream_factory_options defaults;
            defaults.io_uring = rpc::connection_factory_config::io_uring_options{};

            using json::v1::convert::to_json_object;
            return to_json_object(defaults);
        }();
        return options;
    }

    inline rpc::connection_factory::materialise_options_result materialise_io_uring_options(
        const json::v1::object& client_options)
    {
        return rpc::connection_factory::materialise_options(client_options, io_uring_default_options());
    }

    inline uint16_t port_from_options(
        const rpc::connection_factory_config::io_uring_options& options,
        uint16_t = 0)
    {
        return options.port;
    }

    inline loopback_port_range port_range_from_options(const rpc::connection_factory_config::io_uring_options& options)
    {
        loopback_port_range result;
        result.first_port = options.first_port;
        result.last_port = options.last_port;
        return result;
    }

    inline loopback_listen_options listen_options_from_options(
        const rpc::connection_factory_config::io_uring_options& options)
    {
        loopback_listen_options result;
        result.port = port_from_options(options);
        result.port_range = port_range_from_options(options);
        return result;
    }

    struct io_uring_runtime_options_result
    {
        int error_code{rpc::error::OK()};
        rpc::connection_factory_config::stream_factory_options factory_options;
        rpc::io_uring::host_controller::options controller_options;
        ::streaming::io_uring::stream::options stream_options;
        loopback_listen_options listen_options;
        uint16_t port{0};
    };

    namespace detail
    {
        inline io_uring_runtime_options_result make_io_uring_runtime_options(
            rpc::connection_factory_config::stream_factory_options options)
        {
            io_uring_runtime_options_result result;
            result.factory_options = std::move(options);
            if (!result.factory_options.io_uring)
                result.factory_options.io_uring.emplace();

            const auto& io_uring_options = result.factory_options.io_uring.value();
            result.controller_options = io_uring_options.controller;
            result.stream_options = io_uring_options.stream;
            result.listen_options = listen_options_from_options(io_uring_options);
            result.port = io_uring_options.port;
            return result;
        }
    } // namespace detail

} // namespace rpc::io_uring
