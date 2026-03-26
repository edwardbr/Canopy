/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

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

    // A named virtual address used to construct zone_addresses for RPC.
    // The name links tcp_endpoints to this logical identity.
    struct named_virtual_address
    {
        std::string name;
        rpc::zone_address_args args;

        [[nodiscard]] std::string to_string() const;
    };

    // A physical TCP endpoint: an IP address, its family, a port, and the name
    // of the virtual address it maps to.
    //
    // Physical and virtual addresses are decoupled.  A server may listen on
    // an IPv4 address while presenting an IPv6 virtual zone address, and a
    // client may reach the server through a router that translates between the
    // two families.
    struct tcp_endpoint
    {
        std::string name;     // virtual-address name; empty → use default
        ip_address addr = {}; // physical address bytes (all-zero = any/0.0.0.0)
        ip_address_family family = ip_address_family::ipv4;
        uint16_t port = 0;

        // Return the address as a human-readable string.
        //   ipv4 family: dotted-decimal  (e.g. "192.168.1.1")
        //   ipv6 family: full colon-hex  (e.g. "2001:db8::1")
        //   all-zero:    "0.0.0.0"
        [[nodiscard]] std::string to_string() const;
    };

    // Network configuration for an application.
    //
    // virtual_addresses  — logical zone identities, each named.
    // listen_endpoints   — physical (addr, port) pairs to bind on, each
    //                      mapped to a named virtual address.
    // connect_endpoints  — physical (addr, port) pairs to connect to, each
    //                      mapped to a named virtual address.
    struct network_config
    {
        std::vector<named_virtual_address> virtual_addresses;
        std::vector<tcp_endpoint> listen_endpoints;
        std::vector<tcp_endpoint> connect_endpoints;

        // Convenience accessors — return nullptr when the list is empty.
        [[nodiscard]] const tcp_endpoint* first_listen() const noexcept
        {
            return listen_endpoints.empty() ? nullptr : &listen_endpoints.front();
        }
        [[nodiscard]] const tcp_endpoint* first_connect() const noexcept
        {
            return connect_endpoints.empty() ? nullptr : &connect_endpoints.front();
        }

        // Lookup a virtual address by name.  Returns nullptr if not found.
        [[nodiscard]] const named_virtual_address* find_virtual(const std::string& name) const noexcept;

        // Emit all fields via RPC_INFO.
        void log_values() const;
    };

    // Parse a "host:port" string into a tcp_endpoint (no name).
    //   IPv4:  "192.168.1.1:8080"
    //   IPv6:  "[2001:db8::1]:8080"
    //   bare:  "8080"  (0.0.0.0:port)
    // Address family is inferred from the address format.
    // Throws std::invalid_argument on malformed input.
    tcp_endpoint parse_endpoint(const std::string& host_port);

    // Parse a "[name:]host:port" string into a tcp_endpoint.
    //
    // The name token is an identifier — no dots, no brackets, not all-digits.
    // Address family is inferred from the host format:
    //   "[ipv6]:port"  → ipv6
    //   "ipv4:port"    → ipv4
    //   "port"         → ipv4, addr = 0.0.0.0
    // Examples:
    //   "myserver:192.168.1.1:8080"  → name=myserver, IPv4
    //   "myserver:[::1]:8080"         → name=myserver, IPv6
    //   "myserver:8080"               → name=myserver, 0.0.0.0:8080
    //   "192.168.1.1:8080"            → no name, IPv4
    //   "[::1]:8080"                  → no name, IPv6
    //   "8080"                        → no name, 0.0.0.0:8080
    // Throws std::invalid_argument on malformed input.
    tcp_endpoint parse_named_endpoint(const std::string& name_host_port);

    // Holds the args flag handles registered into an ArgumentParser.
    // Must remain alive until get_config() is called (i.e. after ParseCLI()).
    //
    // Flags are organised into two args::Groups:
    //
    //   Virtual zone addresses
    //     All flags are repeatable and matched positionally by occurrence index.
    //     --va-name and --va-type must appear the same number of times (N).
    //     All other flags may have fewer than N entries; missing trailing
    //     entries use their defaults.
    //
    //     --va-name           <identifier>   Name for this virtual address
    //     --va-type           <type>         local | ipv4 | ipv6 | ipv6_tun
    //     --va-prefix         <prefix>       Routing prefix (auto-detect if absent)
    //     --va-subnet-bits    <n>            Subnet field size in bits (default 64)
    //     --va-subnet         <value>        Initial subnet value (default 0);
    //                                        must fit in --va-subnet-bits bits
    //     --va-object-id-bits <n>            Object-id field size in bits (default 64)
    //     --va-object-id      <value>        Initial object_id value (default 0);
    //                                        must fit in --va-object-id-bits bits
    //
    //   Physical network endpoints
    //     --listen   [name:]addr:port     Bind address (address family from format)
    //     --connect  [name:]addr:port     Connect address (address family from format)
    //
    // Usage:
    //   args::ArgumentParser parser("My App");
    //   auto net = canopy::network_config::add_network_args(parser);
    //   parser.ParseCLI(argc, argv);
    //   auto cfg = net.get_config();
    class network_args_context
    {
        args::Group virtual_addresses_group_;
        mutable args::ValueFlagList<std::string> va_name_args_;
        mutable args::ValueFlagList<std::string> va_type_args_;
        mutable args::ValueFlagList<std::string> va_prefix_args_;
        mutable args::ValueFlagList<uint32_t> va_subnet_bits_args_;
        mutable args::ValueFlagList<uint64_t> va_subnet_args_;
        mutable args::ValueFlagList<uint32_t> va_object_id_bits_args_;
        mutable args::ValueFlagList<uint64_t> va_object_id_args_;

        args::Group endpoints_group_;
        mutable args::ValueFlagList<std::string> listen_args_;
        mutable args::ValueFlagList<std::string> connect_args_;

    public:
        explicit network_args_context(args::ArgumentParser& parser);

        // Extract and validate a network_config after ParseCLI().
        // Endpoints without a name default to the first virtual address.
        // Throws std::invalid_argument on invalid or inconsistent arguments.
        [[nodiscard]] network_config get_config() const;
    };

    // Register network args into parser and return a context that must be kept
    // alive until get_config() is called.
    [[nodiscard]] network_args_context add_network_args(args::ArgumentParser& parser);

    // Extract and validate a network_config from a context after ParseCLI().
    network_config get_network_config(const network_args_context& ctx);

    // Convenience: register args, parse, and return config in one step.
    network_config parse_network_args(
        int argc,
        char* argv[],
        args::ArgumentParser& parser);

    // Convert binary ip_address to the legacy uint64_t prefix encoding.
    //   IPv4: 6to4 mapping — 0x2002 << 48 | ipv4_u32 << 16
    //   IPv6: first 8 bytes packed as big-endian uint64_t
    //   all-zero addr: returns 0 (local-only mode)
    uint64_t ip_address_to_uint64(
        const ip_address& addr,
        ip_address_family family);

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

    // Auto-detect a connectable host address from the machine's network interfaces.
    // Fills addr and family.  Returns true on success; on failure fills addr with
    // 127.0.0.1 and sets family to ipv4.
    bool detect_host(
        ip_address& addr,
        ip_address_family& family);
    bool detect_host(
        ip_address& addr,
        ip_address_family& family,
        ip_address_family preferred_family);

    // Parse an explicit host address string (dotted-decimal or colon-hex) into binary.
    // family selects the parser.  Returns true on success, false on malformed input.
    bool parse_ip_address(
        const std::string& str,
        ip_address& addr,
        ip_address_family family);

    // Auto-detect the best routing prefix from the host's network interfaces.
    // Selection priority:
    //   1. First globally-routable unicast IPv6 (non-link-local, non-loopback) — /64 prefix
    //   2. First public IPv4 (not loopback, link-local, or RFC 1918)
    //   3. First private IPv4 (RFC 1918: 10.x, 172.16-31.x, 192.168.x)
    //   4. Returns false — local-only mode, addr left all-zero
    bool detect_routing_prefix(
        ip_address& addr,
        ip_address_family& family);
    bool detect_routing_prefix(
        ip_address& addr,
        ip_address_family& family,
        ip_address_family preferred_family);

} // namespace canopy::network_config
