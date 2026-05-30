/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#ifdef CANOPY_BUILD_COROUTINE

#  include <memory>

#  include <sgx_coroutine_transport/sgx_coroutine_transport_config.h>
#  include <transports/factory.h>

namespace rpc::sgx_coroutine_transport::host
{
    [[nodiscard]] rpc::transport_creation::connect_result connect_transport(
        const rpc::sgx_coroutine_transport::transport_settings& settings,
        std::shared_ptr<rpc::service> service = {});
} // namespace rpc::sgx_coroutine_transport::host

#endif
