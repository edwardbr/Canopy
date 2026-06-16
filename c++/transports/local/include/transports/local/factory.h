/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <memory>

#include <local_transport/local_transport_config.h>
#include <transports/factory.h>

namespace rpc::local_transport
{
    [[nodiscard]] rpc::transport_creation::connect_result connect_transport(
        const transport_settings& settings,
        std::shared_ptr<rpc::service> service = {});
} // namespace rpc::local_transport
