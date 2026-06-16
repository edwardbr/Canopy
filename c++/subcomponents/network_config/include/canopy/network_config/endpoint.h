/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <string>

#include <canopy/network_config/types.h>

namespace canopy::network_config
{
    // Parse a "host:port" string into a tcp_endpoint (no name).
    //   IPv4:  "192.168.1.1:8080"
    //   IPv6:  "[2001:db8::1]:8080"
    //   bare:  "8080"  (0.0.0.0:port)
    // Address family is inferred from the address format.
    // Throws std::invalid_argument on malformed input.
    tcp_endpoint parse_endpoint(const std::string& host_port);

    // Parse a "[name:]host:port" string into a tcp_endpoint.
    //
    // The name token is an identifier: no dots, no brackets, not all-digits.
    // Address family is inferred from the host format.
    // Throws std::invalid_argument on malformed input.
    tcp_endpoint parse_named_endpoint(const std::string& name_host_port);

    // Parse an IPv4 dotted-decimal string into the binary ip_address layout.
    //   bytes[0..3] = IPv4 address, bytes[4..15] = 0
    // Throws std::invalid_argument on malformed input.
    void ipv4_to_ip_address(
        const std::string& dotted_decimal,
        ip_address& addr);

    // Parse an IPv6 colon-hex string and store the /64 network prefix in binary.
    //   bytes[0..7] = first 64 bits of the address, bytes[8..15] = 0
    // Throws std::invalid_argument on malformed input.
    void ipv6_to_ip_address(
        const std::string& colon_hex,
        ip_address& addr);

    // Parse an explicit host address string (dotted-decimal or colon-hex) into binary.
    // family selects the parser. Returns true on success, false on malformed input.
    bool parse_ip_address(
        const std::string& str,
        ip_address& addr,
        ip_address_family family);
} // namespace canopy::network_config
