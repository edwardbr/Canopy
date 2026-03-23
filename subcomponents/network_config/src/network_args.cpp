/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#ifndef _WIN32
#  include <arpa/inet.h>
#endif

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
    // Conversion: binary routing_prefix_addr → internal uint64_t for zone_address
    // ---------------------------------------------------------------------------

    uint64_t ip_address_to_uint64(const ip_address& addr, ip_address_family family)
    {
        // All-zero means local-only mode.
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
            // 6to4-inspired mapping (RFC 3056): 0x2002 << 48 | ipv4_u32 << 16
            uint64_t ipv4 = (static_cast<uint64_t>(addr[0]) << 24) | (static_cast<uint64_t>(addr[1]) << 16)
                            | (static_cast<uint64_t>(addr[2]) << 8) | static_cast<uint64_t>(addr[3]);
            return (UINT64_C(0x2002) << 48) | (ipv4 << 16);
        }

        // IPv6: pack first 8 bytes (the /64 prefix) as big-endian uint64_t.
        uint64_t result = 0;
        for (int i = 0; i < 8; ++i)
            result = (result << 8) | addr[i];
        return result;
    }

    // ---------------------------------------------------------------------------
    // network_config member functions
    // ---------------------------------------------------------------------------

    std::string network_config::get_routing_prefix_string() const
    {
        if (!has_routing_prefix())
            return "127.0.0.1";

#ifndef _WIN32
        if (routing_prefix_family == ip_address_family::ipv4)
        {
            struct in_addr a4;
            uint32_t h = (static_cast<uint32_t>(routing_prefix_addr[0]) << 24)
                         | (static_cast<uint32_t>(routing_prefix_addr[1]) << 16)
                         | (static_cast<uint32_t>(routing_prefix_addr[2]) << 8)
                         | static_cast<uint32_t>(routing_prefix_addr[3]);
            a4.s_addr = htonl(h);
            char buf[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET, &a4, buf, sizeof(buf)))
                return buf;
        }
        else
        {
            // bytes[0..7] are the /64 prefix; bytes[8..15] are zero.
            struct in6_addr a6;
            std::memcpy(a6.s6_addr, routing_prefix_addr.data(), 16);
            char buf[INET6_ADDRSTRLEN];
            if (inet_ntop(AF_INET6, &a6, buf, sizeof(buf)))
                return buf;
        }
#endif
        return "127.0.0.1";
    }

    std::string network_config::get_host_string() const
    {
#ifndef _WIN32
        if (host_family == ip_address_family::ipv4)
        {
            struct in_addr a4;
            uint32_t h = (static_cast<uint32_t>(host_addr[0]) << 24) | (static_cast<uint32_t>(host_addr[1]) << 16)
                         | (static_cast<uint32_t>(host_addr[2]) << 8) | static_cast<uint32_t>(host_addr[3]);
            a4.s_addr = htonl(h);
            char buf[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET, &a4, buf, sizeof(buf)))
                return buf;
        }
        else
        {
            struct in6_addr a6;
            std::memcpy(a6.s6_addr, host_addr.data(), 16);
            char buf[INET6_ADDRSTRLEN];
            if (inet_ntop(AF_INET6, &a6, buf, sizeof(buf)))
                return buf;
        }
#endif
        return "127.0.0.1";
    }

    void network_config::log_values() const
    {
        RPC_INFO("routing prefix : {}", get_routing_prefix_string());
        RPC_INFO("object offset  : {}", object_offset);
        RPC_INFO("host           : {}", get_host_string());
        RPC_INFO("port           : {}", port);
    }

    // ---------------------------------------------------------------------------
    // Routing prefix helpers
    // ---------------------------------------------------------------------------

    void ipv4_to_ip_address(const std::string& dotted_decimal, ip_address& addr)
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

    void ipv6_to_ip_address(const std::string& colon_hex, ip_address& addr)
    {
        // Parse the first 64 bits of an IPv6 address (the /64 network prefix).
        // Handles full form (2001:db8:0:0:...) and compressed (::1, 2001:db8::1).

        // Expand '::' to make parsing uniform.
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

        // Parse up to 4 groups (we only need the first 64 bits = 4 × 16-bit groups).
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
    // network_args_context
    // ---------------------------------------------------------------------------

    network_args_context::network_args_context(args::ArgumentParser& parser)
        : address_family_group_(parser, "Address family for --routing-prefix", args::Group::Validators::AtMostOne)
        , flag_ipv4_(
              address_family_group_, "ipv4", "Interpret --routing-prefix as an IPv4 dotted-decimal address", {'4', "ipv4"})
        , flag_ipv6_(
              address_family_group_, "ipv6", "Interpret --routing-prefix as an IPv6 colon-hex address", {'6', "ipv6"})
        , routing_prefix_(parser,
              "addr",
              "This node's routing prefix. Format determined by -4/-6. "
              "Auto-detected from network interfaces when omitted.",
              {"routing-prefix"},
              "")
        , host_(parser, "addr", "TCP address to bind/connect. \"detect\" derives it from --routing-prefix.", {"host"}, "detect")
        , port_(parser, "n", "TCP port (0 = not specified)", {"port"}, uint32_t{8000})
        , object_offset_(parser,
              "bits",
              "Bit offset where object_id begins in zone_address::local_address (flexible layout, default 64)",
              {"object-offset"},
              uint32_t{64})
    {
    }

    network_config network_args_context::get_config() const
    {
        network_config cfg;
        const bool prefer_ipv4 = args::get(flag_ipv4_);
        const bool prefer_ipv6 = args::get(flag_ipv6_);

        const std::string& prefix_str = args::get(routing_prefix_);
        if (prefix_str.empty())
        {
            if (prefer_ipv4)
            {
                detect_routing_prefix(cfg.routing_prefix_addr, cfg.routing_prefix_family, ip_address_family::ipv4);
            }
            else if (prefer_ipv6)
            {
                detect_routing_prefix(cfg.routing_prefix_addr, cfg.routing_prefix_family, ip_address_family::ipv6);
            }
            else
            {
                detect_routing_prefix(cfg.routing_prefix_addr, cfg.routing_prefix_family);
            }
        }
        else if (prefer_ipv4)
        {
            ipv4_to_ip_address(prefix_str, cfg.routing_prefix_addr);
            cfg.routing_prefix_family = ip_address_family::ipv4;
        }
        else if (prefer_ipv6)
        {
            ipv6_to_ip_address(prefix_str, cfg.routing_prefix_addr);
            cfg.routing_prefix_family = ip_address_family::ipv6;
        }
        else
        {
            // No family flag: infer from format (colon → IPv6, otherwise IPv4).
            if (prefix_str.find(':') != std::string::npos)
            {
                ipv6_to_ip_address(prefix_str, cfg.routing_prefix_addr);
                cfg.routing_prefix_family = ip_address_family::ipv6;
            }
            else
            {
                ipv4_to_ip_address(prefix_str, cfg.routing_prefix_addr);
                cfg.routing_prefix_family = ip_address_family::ipv4;
            }
        }

        cfg.object_offset = static_cast<uint8_t>(args::get(object_offset_));

        constexpr uint8_t object_limit = rpc::zone_address::get_default_local_object_id_size_bits();

        if (cfg.object_offset > object_limit)
        {
            throw std::invalid_argument(
                fmt::format("object-offset ({}) must be in range [0..{}]", cfg.object_offset, object_limit));
        }

        const std::string& host_str = args::get(host_);
        if (host_str == "detect")
        {
            if (prefer_ipv4)
            {
                detect_host(cfg.host_addr, cfg.host_family, ip_address_family::ipv4);
            }
            else if (prefer_ipv6)
            {
                detect_host(cfg.host_addr, cfg.host_family, ip_address_family::ipv6);
            }
            else
            {
                detect_host(cfg.host_addr, cfg.host_family);
            }
        }
        else
        {
            // Infer host family: colon present → IPv6, otherwise IPv4.
            cfg.host_family = (host_str.find(':') != std::string::npos) ? ip_address_family::ipv6 : ip_address_family::ipv4;
            if (!parse_ip_address(host_str, cfg.host_addr, cfg.host_family))
            {
                throw std::invalid_argument(fmt::format("--host: invalid address: {}", host_str));
            }
        }

        cfg.port = static_cast<uint16_t>(args::get(port_));

        return cfg;
    }

    network_args_context add_network_args(args::ArgumentParser& parser)
    {
        return network_args_context{parser};
    }

    network_config get_network_config(const network_args_context& ctx)
    {
        return ctx.get_config();
    }

    network_config parse_network_args(int argc, char* argv[], args::ArgumentParser& parser)
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

} // namespace canopy::network_config
