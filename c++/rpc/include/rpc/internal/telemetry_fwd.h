/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <memory>

namespace rpc::telemetry
{
    class i_telemetry_service;

    std::shared_ptr<i_telemetry_service> get_telemetry_service();
}

#ifndef RPC_TELEMETRY_COMPAT_DECLARED
#  define RPC_TELEMETRY_COMPAT_DECLARED
namespace rpc
{
    using i_telemetry_service = telemetry::i_telemetry_service;

    inline std::shared_ptr<i_telemetry_service> get_telemetry_service()
    {
        return telemetry::get_telemetry_service();
    }
}
#endif
