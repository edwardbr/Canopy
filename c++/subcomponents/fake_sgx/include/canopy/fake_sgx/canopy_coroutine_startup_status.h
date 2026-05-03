/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <cstdint>

struct canopy_coroutine_startup_status
{
    std::uint32_t abi_version;
    std::uint32_t state;
    std::int32_t error_code;
    std::uint32_t requested_workers;
    std::uint32_t attached_workers;
};
