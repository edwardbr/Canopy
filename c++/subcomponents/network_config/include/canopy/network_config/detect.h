/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <canopy/network_config/types.h>

namespace canopy::network_config
{
    // Auto-detect a connectable host address from the machine's network interfaces.
    // Fills addr and family. Returns true on success; on failure fills addr with
    // 127.0.0.1 and sets family to ipv4.
    bool detect_host(
        ip_address& addr,
        ip_address_family& family);
    bool detect_host(
        ip_address& addr,
        ip_address_family& family,
        ip_address_family preferred_family);

    // Auto-detect the best routing prefix from the host's network interfaces.
    // Selection priority:
    //   1. First globally-routable unicast IPv6 (non-link-local, non-loopback): /64 prefix
    //   2. First public IPv4 (not loopback, link-local, or RFC 1918)
    //   3. First private IPv4 (RFC 1918: 10.x, 172.16-31.x, 192.168.x)
    //   4. Returns false: local-only mode, addr left all-zero
    bool detect_routing_prefix(
        ip_address& addr,
        ip_address_family& family);
    bool detect_routing_prefix(
        ip_address& addr,
        ip_address_family& family,
        ip_address_family preferred_family);
} // namespace canopy::network_config
