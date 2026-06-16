/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <stdexcept>
#include <string>

#include <rpc/rpc.h>

#include <canopy/network_config/types.h>

namespace canopy::network_config
{
    // Build a zone_address from a named_virtual_address.
    inline rpc::zone_address make_zone_address(const named_virtual_address& nva)
    {
        return *rpc::zone_address::create(nva.args);
    }

    // Build a zone_address from config, looked up by name.
    // Passing an empty name returns the first virtual address.
    // Throws std::invalid_argument if virtual_addresses is empty or name not found.
    rpc::zone_address get_zone_address(
        const network_config& cfg,
        const std::string& name = "");

    // Build a rpc::zone_id_allocator from a named_virtual_address.
    inline rpc::zone_id_allocator make_allocator(const named_virtual_address& nva)
    {
        return rpc::zone_id_allocator{make_zone_address(nva)};
    }

    // Build a rpc::zone_id_allocator from the first virtual address in a network_config.
    // Throws std::invalid_argument if virtual_addresses is empty.
    inline rpc::zone_id_allocator make_allocator(const network_config& cfg)
    {
        if (cfg.virtual_addresses.empty())
            throw std::invalid_argument("make_allocator: network_config has no virtual_addresses");
        return make_allocator(cfg.virtual_addresses.front());
    }
} // namespace canopy::network_config
