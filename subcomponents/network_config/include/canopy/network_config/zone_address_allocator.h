/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

// The allocator now lives in the rpc core library.
// This header is kept for source compatibility.

#pragma once

#include <rpc/internal/zone_id_allocator.h>

namespace canopy::network_config
{
    // Alias for backward compatibility.
    using zone_address_allocator = rpc::zone_id_allocator;

} // namespace canopy::network_config
