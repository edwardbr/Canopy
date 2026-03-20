/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <array>
#include <cstdint>
#include <string>

#include <rpc/rpc.h>

namespace canopy::network_config
{
    enum class ip_address_family
    {
        ipv4,
        ipv6
    };

    typedef std::array<uint8_t, 16> ip_address;
} // namespace canopy::network_config

#ifdef __clang__
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wshadow"
#  pragma clang diagnostic ignored "-Winconsistent-missing-override"
#elif defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wshadow"
#  pragma GCC diagnostic ignored "-Wsuggest-override"
#endif

#include <args.hxx>

#ifdef __clang__
#  pragma clang diagnostic pop
#elif defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif

namespace canopy::network_config
{

    // Parsed network configuration ready for constructing a rpc::zone_id_allocator.
    //
    // routing_prefix_addr and host_addr are stored in binary form using the same
    // 16-byte layout:
    //   IPv4 family: bytes[0..3] hold the address, bytes[4..15] are zero
    //   IPv6 family: bytes[0..15] hold the full address (routing prefix uses
    //                bytes[0..7] for the /64 network prefix, bytes[8..15] are zero)
    //
    // The subnet range is determined by the zone_address field width and requires
    // no separate configuration.  make_allocator() is the single conversion point
    // between binary network addresses and the rpc::zone_address encoding.
    //
    // object_offset is only used by the flexible zone_address layout
    // (CANOPY_FIXED_ADDRESS_SIZE not set). It marks the boundary within
    // zone_address::local_address:
    //   bits [0 .. object_offset-1]                 subnet
    //   bits [object_offset .. (local_bits - hash_bits - 1)] object
    //   bits [local_bits - hash_bits .. local_bits - 1]      optional hash
    // local_bits defaults to 120 when CANOPY_LOCAL_ADDRESS_SIZE is left at 15.
    struct network_config
    {
        ip_address routing_prefix_addr = {}; // network prefix in binary
        ip_address_family routing_prefix_family = ip_address_family::ipv4;
        ip_address host_addr = {}; // host address in binary
        ip_address_family host_family = ip_address_family::ipv4;
        uint16_t port = 0;          // TCP port; 0 means not specified
        uint8_t object_offset = 64; // bit offset of object field (flexible layout only)

        // Returns true when a non-loopback routing prefix has been set.
        [[nodiscard]] bool has_routing_prefix() const { return routing_prefix_addr != ip_address{}; }

        // Return the routing prefix as a human-readable string.
        //   ipv4 family: dotted-decimal  (e.g. "192.168.1.0")
        //   ipv6 family: colon-hex /64   (e.g. "2001:db8::")
        //   all-zero:    "127.0.0.1"
        [[nodiscard]] std::string get_routing_prefix_string() const;

        // Return host_addr as a human-readable string.
        //   ipv4 family: dotted-decimal  (e.g. "192.168.1.1")
        //   ipv6 family: full colon-hex  (e.g. "2001:db8::1")
        [[nodiscard]] std::string get_host_string() const;

        // Emit all fields via RPC_INFO.
        void log_values() const;
    };

    // Holds the args::Flag/ValueFlag handles registered into an ArgumentParser.
    // Must be kept alive until get_config() is called (i.e. after ParseCLI()).
    //
    // Usage:
    //   args::ArgumentParser parser("My App");
    //   auto net = canopy::network_config::add_network_args(parser);
    //   // ... add other args ...
    //   parser.ParseCLI(argc, argv);
    //   auto cfg = net.get_config();
    class network_args_context
    {
        args::Group address_family_group_;
        args::Flag flag_ipv4_;
        args::Flag flag_ipv6_;
        mutable args::ValueFlag<std::string> routing_prefix_;
        mutable args::ValueFlag<std::string> host_;
        mutable args::ValueFlag<uint32_t> port_;
        mutable args::ValueFlag<uint32_t> object_offset_;

    public:
        explicit network_args_context(args::ArgumentParser& parser);

        // Extract and validate a network_config after ParseCLI().
        // If --routing-prefix was not provided, calls detect_routing_prefix() automatically.
        // Throws std::invalid_argument if any value is out of range.
        [[nodiscard]] network_config get_config() const;
    };

    // Register network args into parser. Returns a context that must be kept alive
    // until get_config() is called.
    //
    //   -4, --ipv4         --routing-prefix is an IPv4 dotted-decimal address (a.b.c.d)
    //   -6, --ipv6         --routing-prefix is an IPv6 colon-hex address (2001:db8::1)
    //   --routing-prefix   this node's routing prefix (auto-detected when omitted)
    //   --object-offset    bit offset where object_id begins in zone_address::local_address
    //                      (flexible layout, default 64)
    //   --host             TCP address to bind/connect; "detect" derives it from routing_prefix
    //   --port             TCP port (default 0 = not specified)
    [[nodiscard]] network_args_context add_network_args(args::ArgumentParser& parser);

    // Extract and validate a network_config from a context after ParseCLI().
    network_config get_network_config(const network_args_context& ctx);

    // Convenience: add args to parser, parse, and return config in one step.
    network_config parse_network_args(int argc, char* argv[], args::ArgumentParser& parser);

    // Convert binary routing_prefix_addr to the uint64_t encoding used by the
    // fixed zone_address layout.
    //   IPv4: 6to4 mapping — 0x2002 << 48 | ipv4_u32 << 16
    //   IPv6: first 8 bytes packed as big-endian uint64_t
    //   all-zero addr: returns 0 (local-only mode)
    uint64_t ip_address_to_uint64(const ip_address& addr, ip_address_family family);

    inline rpc::zone_address get_zone_address(const network_config& cfg)
    {
#ifdef CANOPY_FIXED_ADDRESS_SIZE
        return rpc::zone_address(ip_address_to_uint64(cfg.routing_prefix_addr, cfg.routing_prefix_family), 0);
#else
        return rpc::zone_address(cfg.routing_prefix_addr, cfg.object_offset, 0);
#endif
    }

    // Build a rpc::zone_id_allocator from a network_config.
    // Fixed layout:   converts routing_prefix_addr via ip_address_to_uint64().
    // Flexible layout: stores the full 16-byte host address and uses
    //   cfg.object_offset to describe the local subnet/object boundary.
    // The subnet range is determined by the zone_address field width automatically.
    inline rpc::zone_id_allocator make_allocator(const network_config& cfg)
    {
        return rpc::zone_id_allocator{get_zone_address(cfg)};
    }

    // Parse an IPv4 dotted-decimal string into the binary routing prefix layout.
    //   bytes[0..3] = IPv4 address, bytes[4..15] = 0
    // Throws std::invalid_argument on malformed input.
    void ipv4_to_ip_address(const std::string& dotted_decimal, ip_address& addr);

    // Parse an IPv6 colon-hex string and store the /64 network prefix in binary.
    //   bytes[0..7] = first 64 bits of the address, bytes[8..15] = 0
    // Throws std::invalid_argument on malformed input.
    void ipv6_to_ip_address(const std::string& colon_hex, ip_address& addr);

    // Auto-detect a connectable host address from the machine's network interfaces,
    // using the same priority ordering as detect_routing_prefix().
    // Fills addr (16 bytes) and family. Returns true on success; on failure fills
    // addr with 127.0.0.1 and sets family to ipv4.
    bool detect_host(ip_address& addr, ip_address_family& family);
    bool detect_host(ip_address& addr, ip_address_family& family, ip_address_family preferred_family);

    // Parse an explicit host address string (dotted-decimal or colon-hex) into binary.
    // family selects the parser; pass ipv4 for a.b.c.d, ipv6 for colon-hex.
    // Returns true on success, false on malformed input.
    bool parse_ip_address(const std::string& str, ip_address& addr, ip_address_family family);

    // Auto-detect the best routing prefix from the host's network interfaces.
    // Selection priority:
    //   1. First globally-routable unicast IPv6 (non-link-local, non-loopback) — /64 prefix stored
    //   2. First public IPv4 (not loopback, link-local, or RFC 1918)
    //   3. First private IPv4 (RFC 1918: 10.x, 172.16-31.x, 192.168.x)
    //   4. Returns false — local-only mode, addr left all-zero
    bool detect_routing_prefix(ip_address& addr, ip_address_family& family);
    bool detect_routing_prefix(ip_address& addr, ip_address_family& family, ip_address_family preferred_family);

} // namespace canopy::network_config
