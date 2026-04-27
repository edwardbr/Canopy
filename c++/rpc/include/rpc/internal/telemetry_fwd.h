/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#ifdef CANOPY_USE_TELEMETRY

#  include <memory>

namespace rpc::telemetry
{
    class i_telemetry_service;

    std::shared_ptr<i_telemetry_service> get_telemetry_service();
}

#endif
