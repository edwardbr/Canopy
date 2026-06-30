/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <string>

#include <canopy/rest/connection.h>
#include <connection_factory/connection_factory.h>

namespace canopy::rest
{
    struct connection_factory_settings
    {
        std::string base_stream_type;
        rpc::connection_factory::connection_settings stream_connection;
        rpc::connection_factory::context factory_context{rpc::connection_factory::default_context()};
    };

    [[nodiscard]] rpc::connection_factory::connection_settings make_stream_connection_settings(
        const connection_settings& settings,
        const connection_factory_settings& factory_settings = {});

    [[nodiscard]] stream_connector make_connection_factory_connector(connection_factory_settings factory_settings = {});

    void use_connection_factory(
        connection_settings& settings,
        connection_factory_settings factory_settings = {});
} // namespace canopy::rest
