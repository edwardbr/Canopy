/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#ifndef CANOPY_BUILD_COROUTINE

#  include <memory>

#  include <sgx_blocking_transport/sgx_blocking_transport_config.h>
#  include <transports/factory.h>

namespace rpc::sgx_blocking_transport
{
    [[nodiscard]] rpc::transport_creation::connect_result connect_transport(
        const transport_settings& settings,
        std::shared_ptr<rpc::service> service = {});
} // namespace rpc::sgx_blocking_transport

#endif
