/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <array>
#include <cstdint>

#include <network_config/network_config.h>

namespace canopy::network_config
{
    using ip_address = std::array<uint8_t, 16>;
} // namespace canopy::network_config
