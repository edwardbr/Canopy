/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#ifndef _WIN32
#  include <arpa/inet.h>
#endif

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#include <fmt/format.h>

#include <rpc/rpc.h>

#include <canopy/network_config/network_args.h>

namespace canopy::network_config
{

    // ---------------------------------------------------------------------------
    // Conversion: binary ip_address → internal uint64_t for zone_address
    // ---------------------------------------------------------------------------

    uint64_t ip_address_to_uint64(
        const ip_address& addr,
        ip_address_family family)
    {
        bool all_zero = true;
        for (auto b : addr)
        {
            if (b != 0)
            {
                all_zero = false;
                break;
            }
        }
        if (all_zero)
            return 0;

        if (family == ip_address_family::ipv4)
        {
            uint64_t ipv4 = (static_cast<uint64_t>(addr[0]) << 24) | (static_cast<uint64_t>(addr[1]) << 16)
                            | (static_cast<uint64_t>(addr[2]) << 8) | static_cast<uint64_t>(addr[3]);
            return (UINT64_C(0x2002) << 48) | (ipv4 << 16);
        }

        uint64_t result = 0;
        for (int i = 0; i < 8; ++i)
            result = (result << 8) | addr[i];
        return result;
    }

    // ---------------------------------------------------------------------------
    // Helpers
    // ---------------------------------------------------------------------------

    namespace
    {
        std::string ip_address_to_string(
            const ip_address& addr,
            ip_address_family family)
        {
#ifndef _WIN32
            if (family == ip_address_family::ipv4)
            {
                struct in_addr a4;
                uint32_t h = (static_cast<uint32_t>(addr[0]) << 24) | (static_cast<uint32_t>(addr[1]) << 16)
                             | (static_cast<uint32_t>(addr[2]) << 8) | static_cast<uint32_t>(addr[3]);
                a4.s_addr = htonl(h);
                char buf[INET_ADDRSTRLEN];
                if (inet_ntop(AF_INET, &a4, buf, sizeof(buf)))
                    return buf;
            }
            else
            {
                struct in6_addr a6;
                std::memcpy(a6.s6_addr, addr.data(), 16);
                char buf[INET6_ADDRSTRLEN];
                if (inet_ntop(AF_INET6, &a6, buf, sizeof(buf)))
                    return buf;
            }
#endif
            return "0.0.0.0";
        }
    } // namespace

    // ---------------------------------------------------------------------------
    // named_virtual_address
    // ---------------------------------------------------------------------------

    std::string named_virtual_address::to_string() const
    {
        std::string type_str;
        switch (args.type)
        {
        case rpc::address_type::local:
            type_str = "local";
            break;
        case rpc::address_type::ipv4:
            type_str = "ipv4";
            break;
        case rpc::address_type::ipv6:
            type_str = "ipv6";
            break;
        case rpc::address_type::ipv6_tun:
            type_str = "ipv6_tun";
            break;
        }
        return fmt::format(
            "{}:{}:prefix_len={}:subnet_bits={}:subnet={}:obj_bits={}:obj_id={}",
            name,
            type_str,
            args.routing_prefix.size(),
            args.subnet_size_bits,
            args.subnet,
            args.object_id_size_bits,
            args.object_id);
    }

    // ---------------------------------------------------------------------------
    // tcp_endpoint
    // ---------------------------------------------------------------------------

    std::string tcp_endpoint::to_string() const
    {
        return ip_address_to_string(addr, family);
    }

    // ---------------------------------------------------------------------------
    // network_config
    // ---------------------------------------------------------------------------

    const named_virtual_address* network_config::find_virtual(const std::string& vname) const noexcept
    {
        for (const auto& va : virtual_addresses)
            if (va.name == vname)
                return &va;
        return nullptr;
    }

    void network_config::log_values() const
    {
        if (virtual_addresses.empty())
        {
            RPC_INFO("virtual addresses: (none)");
        }
        else
        {
            for (size_t i = 0; i < virtual_addresses.size(); ++i)
            {
                const auto& va = virtual_addresses[i];
                RPC_INFO(
                    "virtual[{}]      : name={} type={} subnet_bits={} subnet={} obj_bits={} obj_id={} prefix_len={}",
                    i,
                    va.name,
                    static_cast<int>(va.args.type),
                    va.args.subnet_size_bits,
                    va.args.subnet,
                    va.args.object_id_size_bits,
                    va.args.object_id,
                    va.args.routing_prefix.size());
            }
        }

        if (listen_endpoints.empty())
        {
            RPC_INFO("listen endpoints : (none)");
        }
        else
        {
            for (size_t i = 0; i < listen_endpoints.size(); ++i)
            {
                const auto& ep = listen_endpoints[i];
                RPC_INFO(
                    "listen[{}]        : name={} {}:{}", i, ep.name.empty() ? "(default)" : ep.name, ep.to_string(), ep.port);
            }
        }

        if (connect_endpoints.empty())
        {
            RPC_INFO("connect endpoints: (none)");
        }
        else
        {
            for (size_t i = 0; i < connect_endpoints.size(); ++i)
            {
                const auto& ep = connect_endpoints[i];
                RPC_INFO(
                    "connect[{}]       : name={} {}:{}", i, ep.name.empty() ? "(default)" : ep.name, ep.to_string(), ep.port);
            }
        }
    }

    // ---------------------------------------------------------------------------
    // Routing prefix helpers
    // ---------------------------------------------------------------------------

    void ipv4_to_ip_address(
        const std::string& dotted_decimal,
        ip_address& addr)
    {
        unsigned int a = 0;
        unsigned int b = 0;
        unsigned int c = 0;
        unsigned int d = 0;
        char trailing = 0;
        int matched = std::sscanf(dotted_decimal.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &trailing);
        if (matched != 4 || a > 255 || b > 255 || c > 255 || d > 255)
        {
            throw std::invalid_argument(fmt::format("ipv4_to_ip_address: invalid IPv4 address: {}", dotted_decimal));
        }
        addr = {};
        addr[0] = static_cast<uint8_t>(a);
        addr[1] = static_cast<uint8_t>(b);
        addr[2] = static_cast<uint8_t>(c);
        addr[3] = static_cast<uint8_t>(d);
    }

    void ipv6_to_ip_address(
        const std::string& colon_hex,
        ip_address& addr)
    {
        std::string expanded = colon_hex;
        auto dc = expanded.find("::");
        if (dc != std::string::npos)
        {
            std::string before = expanded.substr(0, dc);
            std::string after = expanded.substr(dc + 2);

            int groups_before = before.empty() ? 0 : 1;
            for (char ch : before)
                if (ch == ':')
                    ++groups_before;

            int groups_after = after.empty() ? 0 : 1;
            for (char ch : after)
                if (ch == ':')
                    ++groups_after;

            int zero_groups = 8 - groups_before - groups_after;
            std::string zeros;
            for (int i = 0; i < zero_groups; ++i)
            {
                if (i > 0)
                    zeros += ':';
                zeros += "0";
            }
            expanded = (before.empty() ? "" : before + ":") + zeros + (after.empty() ? "" : ":" + after);
        }

        addr = {};
        std::istringstream ss(expanded);
        std::string group;
        int g = 0;
        while (std::getline(ss, group, ':') && g < 4)
        {
            if (group.empty())
                group = "0";
            try
            {
                uint64_t val = std::stoull(group, nullptr, 16);
                if (val > 0xFFFF)
                {
                    throw std::invalid_argument(fmt::format("ipv6_to_ip_address: group overflow in: {}", colon_hex));
                }
                addr[g * 2] = static_cast<uint8_t>(val >> 8);
                addr[g * 2 + 1] = static_cast<uint8_t>(val);
            }
            catch (const std::invalid_argument&)
            {
                throw;
            }
            catch (const std::exception&)
            {
                throw std::invalid_argument(fmt::format("ipv6_to_ip_address: invalid IPv6 address: {}", colon_hex));
            }
            ++g;
        }

        if (g < 4)
        {
            throw std::invalid_argument(fmt::format("ipv6_to_ip_address: too few groups in: {}", colon_hex));
        }
    }

    // ---------------------------------------------------------------------------
    // parse_endpoint
    // ---------------------------------------------------------------------------

    tcp_endpoint parse_endpoint(const std::string& host_port)
    {
        tcp_endpoint ep;

        if (host_port.empty())
            throw std::invalid_argument("parse_endpoint: empty endpoint string");

        // Pure port: "8080" → 0.0.0.0:8080  (IPv4)
        {
            bool all_digit = std::all_of(host_port.begin(), host_port.end(), [](char c) { return c >= '0' && c <= '9'; });
            if (all_digit)
            {
                unsigned long val = std::stoul(host_port);
                if (val > 65535)
                    throw std::invalid_argument(fmt::format("parse_endpoint: port out of range: {}", host_port));
                ep.port = static_cast<uint16_t>(val);
                ep.family = ip_address_family::ipv4;
                return ep;
            }
        }

        // Bracketed IPv6: "[2001:db8::1]:8080"
        if (host_port.front() == '[')
        {
            auto close = host_port.find(']');
            if (close == std::string::npos)
                throw std::invalid_argument(fmt::format("parse_endpoint: missing ']' in IPv6 endpoint: {}", host_port));

            std::string addr_str = host_port.substr(1, close - 1);
            std::string rest = host_port.substr(close + 1);

            if (rest.empty() || rest[0] != ':')
                throw std::invalid_argument(fmt::format("parse_endpoint: missing port after ']' in: {}", host_port));

            unsigned long val = std::stoul(rest.substr(1));
            if (val > 65535)
                throw std::invalid_argument(fmt::format("parse_endpoint: port out of range: {}", host_port));

            ipv6_to_ip_address(addr_str, ep.addr);
            ep.family = ip_address_family::ipv6;
            ep.port = static_cast<uint16_t>(val);
            return ep;
        }

        // IPv4:port — find the last colon
        auto colon = host_port.rfind(':');
        if (colon == std::string::npos)
            throw std::invalid_argument(fmt::format("parse_endpoint: no port in endpoint: {}", host_port));

        std::string addr_str = host_port.substr(0, colon);
        unsigned long val = std::stoul(host_port.substr(colon + 1));
        if (val > 65535)
            throw std::invalid_argument(fmt::format("parse_endpoint: port out of range: {}", host_port));

        ipv4_to_ip_address(addr_str, ep.addr);
        ep.family = ip_address_family::ipv4;
        ep.port = static_cast<uint16_t>(val);
        return ep;
    }

    // ---------------------------------------------------------------------------
    // parse_named_endpoint
    // ---------------------------------------------------------------------------

    // A name token is an identifier: no dots (would be IPv4), no brackets,
    // and not purely digits (would be a bare port).
    tcp_endpoint parse_named_endpoint(const std::string& raw)
    {
        if (raw.empty())
            throw std::invalid_argument("parse_named_endpoint: empty string");

        // Bracketed IPv6 literal — no name prefix.
        if (raw.front() == '[')
            return parse_endpoint(raw);

        auto first_colon = raw.find(':');
        if (first_colon == std::string::npos)
            return parse_endpoint(raw); // bare port or error

        const std::string prefix = raw.substr(0, first_colon);

        // A name has no dots, no brackets, and is not purely numeric.
        const bool is_name = !prefix.empty() && prefix.find('.') == std::string::npos
                             && prefix.find('[') == std::string::npos
                             && !std::all_of(prefix.begin(), prefix.end(), [](char c) { return c >= '0' && c <= '9'; });

        if (!is_name)
            return parse_endpoint(raw);

        tcp_endpoint ep = parse_endpoint(raw.substr(first_colon + 1));
        ep.name = prefix;
        return ep;
    }

    // ---------------------------------------------------------------------------
    // network_args_context — constructor
    // ---------------------------------------------------------------------------

    network_args_context::network_args_context(args::ArgumentParser& parser)
        : virtual_addresses_group_(
              parser,
              "Virtual zone addresses\n"
              "  All flags are repeatable and matched positionally by index.\n"
              "  --va-name and --va-type must appear the same number of times (N).\n"
              "  All other flags may have fewer than N entries; missing trailing\n"
              "  entries use their defaults (auto-detect, 64, 0, 64, 0).")
        , va_name_args_(
              virtual_addresses_group_,
              "identifier",
              "Name for this virtual address (e.g. \"server\", \"gateway\"). "
              "Required — one per virtual address.",
              {"va-name"})
        , va_type_args_(
              virtual_addresses_group_,
              "local|ipv4|ipv6|ipv6_tun",
              "Zone address type for this virtual address. "
              "Required — one per virtual address.",
              {"va-type"})
        , va_prefix_args_(
              virtual_addresses_group_,
              "routing-prefix",
              "Routing prefix for this virtual address.\n"
              "  IPv4 example: 192.168.1.0\n"
              "  IPv6 example: 2001:db8::\n"
              "  Omit (or supply fewer than --va-name count) to auto-detect from\n"
              "  the local network interfaces.",
              {"va-prefix"})
        , va_subnet_bits_args_(
              virtual_addresses_group_,
              "bits",
              "Subnet field size in bits for this virtual address (default: 64). "
              "Supply fewer than --va-name count to use the default for trailing entries.",
              {"va-subnet-bits"})
        , va_subnet_args_(
              virtual_addresses_group_,
              "value",
              "Initial subnet value for this virtual address (default: 0). "
              "Must fit within the number of bits set by --va-subnet-bits.",
              {"va-subnet"})
        , va_object_id_bits_args_(
              virtual_addresses_group_,
              "bits",
              "Object-id field size in bits for this virtual address (default: 64). "
              "Supply fewer than --va-name count to use the default for trailing entries.",
              {"va-object-id-bits"})
        , va_object_id_args_(
              virtual_addresses_group_,
              "value",
              "Initial object_id value for this virtual address (default: 0). "
              "Must fit within the number of bits set by --va-object-id-bits.",
              {"va-object-id"})
        , endpoints_group_(
              parser,
              "Physical network endpoints\n"
              "  Address family is inferred from the address format:\n"
              "    IPv6: [2001:db8::1]:port\n"
              "    IPv4: 192.168.1.1:port\n"
              "    Any:  port  (binds 0.0.0.0, IPv4)")
        , listen_args_(
              endpoints_group_,
              "[va-name:]addr:port",
              "Physical address/port to bind and listen on.\n"
              "  va-name : optional virtual-address name (defaults to first defined)\n"
              "  Examples:\n"
              "    --listen server:0.0.0.0:8080\n"
              "    --listen gateway:[::]:443\n"
              "    --listen 9090",
              {"listen"})
        , connect_args_(
              endpoints_group_,
              "[va-name:]addr:port",
              "Physical address/port to connect to.\n"
              "  va-name : optional virtual-address name (defaults to first defined)\n"
              "  Examples:\n"
              "    --connect server:192.168.1.100:8080\n"
              "    --connect gateway:[2001:db8::1]:443",
              {"connect"})
    {
    }

    // ---------------------------------------------------------------------------
    // Parsing helpers (file-scope)
    // ---------------------------------------------------------------------------

    namespace
    {
        rpc::address_type parse_address_type(const std::string& type_str)
        {
            if (type_str == "local")
                return rpc::address_type::local;
            if (type_str == "ipv4")
                return rpc::address_type::ipv4;
            if (type_str == "ipv6")
                return rpc::address_type::ipv6;
            if (type_str == "ipv6_tun")
                return rpc::address_type::ipv6_tun;
            throw std::invalid_argument(
                fmt::format("--va-type: unknown type '{}' (expected local|ipv4|ipv6|ipv6_tun)", type_str));
        }

        // Returns true when value fits in the given number of bits.
        // Handles the bits==64 edge case (shifting uint64_t by 64 is UB).
        bool fits_in_bits(
            uint64_t value,
            uint8_t bits)
        {
            if (bits >= 64)
                return true;
            return value < (UINT64_C(1) << bits);
        }

        // Build one named_virtual_address from its already-separated fields.
        // prefix_str is empty when the prefix should be auto-detected.
        void build_virtual_address(
            const std::string& name,
            const std::string& type_str,
            const std::string& prefix_str,
            uint8_t subnet_bits,
            uint64_t subnet,
            uint8_t object_id_bits,
            uint64_t object_id,
            std::vector<named_virtual_address>& out)
        {
            if (name.empty())
                throw std::invalid_argument("--va-name: empty name");

            const auto type = parse_address_type(type_str);

            ip_address prefix_addr = {};
            ip_address_family prefix_family = ip_address_family::ipv4;

            if (type == rpc::address_type::local)
            {
                // local type has no network prefix
            }
            else if (!prefix_str.empty())
            {
                // Parse the explicit prefix; infer family from type.
                if (type == rpc::address_type::ipv4)
                {
                    ipv4_to_ip_address(prefix_str, prefix_addr);
                    prefix_family = ip_address_family::ipv4;
                }
                else // ipv6 / ipv6_tun
                {
                    ipv6_to_ip_address(prefix_str, prefix_addr);
                    prefix_family = ip_address_family::ipv6;
                }
            }
            else
            {
                // Auto-detect: prefer the family that matches the address type.
                const ip_address_family preferred
                    = (type == rpc::address_type::ipv6 || type == rpc::address_type::ipv6_tun) ? ip_address_family::ipv6
                                                                                               : ip_address_family::ipv4;
                detect_routing_prefix(prefix_addr, prefix_family, preferred);
            }

            rpc::zone_address_args cargs;
            cargs.version = rpc::default_values::version_3;
            cargs.type = type;
            cargs.port = 0; // port lives in tcp_endpoint, not in the virtual address definition
            cargs.subnet_size_bits = subnet_bits;
            cargs.subnet = subnet;
            cargs.object_id_size_bits = object_id_bits;
            cargs.object_id = object_id;

            if (type != rpc::address_type::local)
            {
                cargs.routing_prefix = std::vector<uint8_t>(
                    prefix_addr.begin(),
                    prefix_family == ip_address_family::ipv4 ? prefix_addr.begin() + 4 : prefix_addr.end());
            }

            named_virtual_address nva;
            nva.name = name;
            nva.args = std::move(cargs);
            out.push_back(std::move(nva));
        }
    } // namespace

    // ---------------------------------------------------------------------------
    // network_args_context::get_config
    // ---------------------------------------------------------------------------

    network_config network_args_context::get_config() const
    {
        network_config cfg;

        const auto& names = args::get(va_name_args_);
        const auto& types = args::get(va_type_args_);
        const auto& prefixes = args::get(va_prefix_args_);
        const auto& subnet_bits_list = args::get(va_subnet_bits_args_);
        const auto& subnet_list = args::get(va_subnet_args_);
        const auto& obj_id_bits_list = args::get(va_object_id_bits_args_);
        const auto& obj_id_list = args::get(va_object_id_args_);

        // Validate that required per-VA flags appear the same number of times.
        if (names.size() != types.size())
            throw std::invalid_argument(
                fmt::format(
                    "--va-name ({}) and --va-type ({}) must be specified the same number of times",
                    names.size(),
                    types.size()));

        // Optional per-VA flags must not outnumber --va-name entries.
        for (const auto& [flag, count] : {
                 std::pair<const char*, size_t>{"--va-prefix", prefixes.size()},
                 {"--va-subnet-bits", subnet_bits_list.size()},
                 {"--va-subnet", subnet_list.size()},
                 {"--va-object-id-bits", obj_id_bits_list.size()},
                 {"--va-object-id", obj_id_list.size()},
             })
        {
            if (count > names.size())
                throw std::invalid_argument(
                    fmt::format("{} ({}) specified more times than --va-name ({})", flag, count, names.size()));
        }

        constexpr uint8_t max_bits = rpc::default_values::default_object_id_size_bits;

        for (size_t i = 0; i < names.size(); ++i)
        {
            const std::string& prefix = (i < prefixes.size()) ? prefixes[i] : "";

            const uint32_t raw_subnet_bits = (i < subnet_bits_list.size())
                                                 ? subnet_bits_list[i]
                                                 : uint32_t{rpc::default_values::default_subnet_size_bits};
            const uint32_t raw_obj_id_bits = (i < obj_id_bits_list.size())
                                                 ? obj_id_bits_list[i]
                                                 : uint32_t{rpc::default_values::default_object_id_size_bits};

            if (raw_subnet_bits > max_bits)
                throw std::invalid_argument(
                    fmt::format("--va-subnet-bits[{}] value {} exceeds maximum {}", i, raw_subnet_bits, max_bits));

            if (raw_obj_id_bits > max_bits)
                throw std::invalid_argument(
                    fmt::format("--va-object-id-bits[{}] value {} exceeds maximum {}", i, raw_obj_id_bits, max_bits));

            const auto subnet_bits = static_cast<uint8_t>(raw_subnet_bits);
            const auto obj_id_bits = static_cast<uint8_t>(raw_obj_id_bits);

            const uint64_t subnet_val = (i < subnet_list.size()) ? subnet_list[i] : 0;
            const uint64_t obj_id_val = (i < obj_id_list.size()) ? obj_id_list[i] : 0;

            if (!fits_in_bits(subnet_val, subnet_bits))
                throw std::invalid_argument(
                    fmt::format("--va-subnet[{}] value {} does not fit in {} bits", i, subnet_val, subnet_bits));

            if (!fits_in_bits(obj_id_val, obj_id_bits))
                throw std::invalid_argument(
                    fmt::format("--va-object-id[{}] value {} does not fit in {} bits", i, obj_id_val, obj_id_bits));

            build_virtual_address(
                names[i], types[i], prefix, subnet_bits, subnet_val, obj_id_bits, obj_id_val, cfg.virtual_addresses);
        }

        for (const auto& s : args::get(listen_args_))
            cfg.listen_endpoints.push_back(parse_named_endpoint(s));

        for (const auto& s : args::get(connect_args_))
            cfg.connect_endpoints.push_back(parse_named_endpoint(s));

        // Endpoints without an explicit name default to the first virtual address.
        if (!cfg.virtual_addresses.empty())
        {
            const std::string& default_name = cfg.virtual_addresses.front().name;
            for (auto& ep : cfg.listen_endpoints)
                if (ep.name.empty())
                    ep.name = default_name;
            for (auto& ep : cfg.connect_endpoints)
                if (ep.name.empty())
                    ep.name = default_name;
        }

        return cfg;
    }

    // ---------------------------------------------------------------------------
    // Free functions
    // ---------------------------------------------------------------------------

    network_args_context add_network_args(args::ArgumentParser& parser)
    {
        return network_args_context{parser};
    }

    network_config get_network_config(const network_args_context& ctx)
    {
        return ctx.get_config();
    }

    network_config parse_network_args(
        int argc,
        char* argv[],
        args::ArgumentParser& parser)
    {
        network_args_context ctx{parser};
        try
        {
            parser.ParseCLI(argc, argv);
        }
        catch (const args::Help&)
        {
            std::cout << parser;
        }
        catch (const args::ParseError& e)
        {
            std::cerr << e.what() << "\n" << parser;
            throw;
        }
        return ctx.get_config();
    }

    rpc::zone_address get_zone_address(
        const network_config& cfg,
        const std::string& name)
    {
        if (cfg.virtual_addresses.empty())
            throw std::invalid_argument("get_zone_address: network_config has no virtual_addresses");
        if (name.empty())
            return make_zone_address(cfg.virtual_addresses.front());
        const auto* va = cfg.find_virtual(name);
        if (!va)
            throw std::invalid_argument(fmt::format("get_zone_address: virtual address '{}' not found", name));
        return make_zone_address(*va);
    }

} // namespace canopy::network_config
