/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#ifndef _WIN32
#  include <ifaddrs.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#endif

#include <array>
#include <cstring>
#include <cstdint>
#include <string>

#include <rpc/rpc.h>

#include <canopy/network_config/network_args.h>

namespace canopy::network_config
{

    namespace
    {
        // Returns true when an IPv4 address (host byte order) is globally routable:
        // not loopback (127.x), not link-local (169.254.x), not RFC 1918 private.
        bool ipv4_is_public(uint32_t addr)
        {
            if ((addr >> 24) == 127)
                return false; // loopback 127.x
            if ((addr >> 24) == 10)
                return false; // RFC 1918 10.x
            if ((addr >> 16) == 0xA9FE)
                return false; // link-local 169.254.x
            if ((addr >> 20) == (0xAC10 >> 4))
                return false; // RFC 1918 172.16-31.x
            if ((addr >> 16) == 0xC0A8)
                return false; // RFC 1918 192.168.x
            return true;
        }

        bool ipv4_is_private_rfc1918(uint32_t addr)
        {
            if ((addr >> 24) == 10)
                return true;
            if ((addr >> 20) == (0xAC10 >> 4))
                return true;
            if ((addr >> 16) == 0xC0A8)
                return true;
            return false;
        }

        // Returns true when an IPv6 address is globally routable:
        // not loopback (::1), not link-local (fe80::/10).
        bool ipv6_is_global(const uint8_t* addr16)
        {
            static const uint8_t loopback[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
            if (std::memcmp(addr16, loopback, 16) == 0)
                return false;
            if (addr16[0] == 0xFE && (addr16[1] & 0xC0) == 0x80)
                return false; // link-local fe80::/10
            return true;
        }

        void store_ipv4_bytes(
            uint32_t host_order,
            ip_address& out)
        {
            out = {};
            out[0] = static_cast<uint8_t>(host_order >> 24);
            out[1] = static_cast<uint8_t>(host_order >> 16);
            out[2] = static_cast<uint8_t>(host_order >> 8);
            out[3] = static_cast<uint8_t>(host_order);
        }

        bool detect_routing_prefix_impl(
            ip_address& addr,
            ip_address_family& family,
            const ip_address_family* preferred_family)
        {
#ifndef _WIN32
            ifaddrs* ifa_list = nullptr;
            if (getifaddrs(&ifa_list) != 0)
                return false;

            ip_address best_ipv6 = {};
            bool found_ipv6 = false;
            ip_address best_public_ipv4 = {};
            bool found_public_ipv4 = false;
            ip_address best_private_ipv4 = {};
            bool found_private_ipv4 = false;

            for (ifaddrs* ifa = ifa_list; ifa != nullptr; ifa = ifa->ifa_next)
            {
                if (!ifa->ifa_addr)
                    continue;

                if (ifa->ifa_addr->sa_family == AF_INET6 && !found_ipv6)
                {
                    auto* sa6 = reinterpret_cast<sockaddr_in6*>(ifa->ifa_addr);
                    const uint8_t* a = sa6->sin6_addr.s6_addr;
                    if (ipv6_is_global(a))
                    {
                        best_ipv6 = {};
                        std::memcpy(best_ipv6.data(), a, 8);
                        found_ipv6 = true;
                    }
                }
                else if (ifa->ifa_addr->sa_family == AF_INET)
                {
                    auto* sa4 = reinterpret_cast<sockaddr_in*>(ifa->ifa_addr);
                    uint32_t a = ntohl(sa4->sin_addr.s_addr);

                    if (!found_public_ipv4 && ipv4_is_public(a))
                    {
                        store_ipv4_bytes(a, best_public_ipv4);
                        found_public_ipv4 = true;
                    }
                    else if (!found_private_ipv4 && ipv4_is_private_rfc1918(a))
                    {
                        store_ipv4_bytes(a, best_private_ipv4);
                        found_private_ipv4 = true;
                    }
                }
            }

            freeifaddrs(ifa_list);

            if (preferred_family)
            {
                if (*preferred_family == ip_address_family::ipv6 && found_ipv6)
                {
                    addr = best_ipv6;
                    family = ip_address_family::ipv6;
                    return true;
                }
                if (*preferred_family == ip_address_family::ipv4)
                {
                    if (found_public_ipv4)
                    {
                        addr = best_public_ipv4;
                        family = ip_address_family::ipv4;
                        return true;
                    }
                    if (found_private_ipv4)
                    {
                        addr = best_private_ipv4;
                        family = ip_address_family::ipv4;
                        return true;
                    }
                }
                return false;
            }

            if (found_ipv6)
            {
                addr = best_ipv6;
                family = ip_address_family::ipv6;
                return true;
            }
            if (found_public_ipv4)
            {
                addr = best_public_ipv4;
                family = ip_address_family::ipv4;
                return true;
            }
            if (found_private_ipv4)
            {
                addr = best_private_ipv4;
                family = ip_address_family::ipv4;
                return true;
            }
#endif
            return false;
        }

        bool detect_host_impl(
            ip_address& addr,
            ip_address_family& family,
            const ip_address_family* preferred_family)
        {
#ifndef _WIN32
            ifaddrs* ifa_list = nullptr;
            if (getifaddrs(&ifa_list) != 0)
            {
                addr = {};
                addr[0] = 127;
                addr[3] = 1;
                family = ip_address_family::ipv4;
                return false;
            }

            ip_address best_ipv6 = {};
            bool found_ipv6 = false;
            ip_address best_public_ipv4 = {};
            bool found_public_ipv4 = false;
            ip_address best_private_ipv4 = {};
            bool found_private_ipv4 = false;

            for (ifaddrs* ifa = ifa_list; ifa != nullptr; ifa = ifa->ifa_next)
            {
                if (!ifa->ifa_addr)
                    continue;

                if (ifa->ifa_addr->sa_family == AF_INET6 && !found_ipv6)
                {
                    auto* sa6 = reinterpret_cast<sockaddr_in6*>(ifa->ifa_addr);
                    const uint8_t* a = sa6->sin6_addr.s6_addr;
                    if (ipv6_is_global(a))
                    {
                        std::memcpy(best_ipv6.data(), a, 16);
                        found_ipv6 = true;
                    }
                }
                else if (ifa->ifa_addr->sa_family == AF_INET)
                {
                    auto* sa4 = reinterpret_cast<sockaddr_in*>(ifa->ifa_addr);
                    uint32_t a = ntohl(sa4->sin_addr.s_addr);

                    if (!found_public_ipv4 && ipv4_is_public(a))
                    {
                        store_ipv4_bytes(a, best_public_ipv4);
                        found_public_ipv4 = true;
                    }
                    else if (!found_private_ipv4 && ipv4_is_private_rfc1918(a))
                    {
                        store_ipv4_bytes(a, best_private_ipv4);
                        found_private_ipv4 = true;
                    }
                }
            }

            freeifaddrs(ifa_list);

            if (preferred_family)
            {
                if (*preferred_family == ip_address_family::ipv6 && found_ipv6)
                {
                    addr = best_ipv6;
                    family = ip_address_family::ipv6;
                    return true;
                }
                if (*preferred_family == ip_address_family::ipv4)
                {
                    if (found_public_ipv4)
                    {
                        addr = best_public_ipv4;
                        family = ip_address_family::ipv4;
                        return true;
                    }
                    if (found_private_ipv4)
                    {
                        addr = best_private_ipv4;
                        family = ip_address_family::ipv4;
                        return true;
                    }
                }
            }
            else
            {
                if (found_ipv6)
                {
                    addr = best_ipv6;
                    family = ip_address_family::ipv6;
                    return true;
                }
                if (found_public_ipv4)
                {
                    addr = best_public_ipv4;
                    family = ip_address_family::ipv4;
                    return true;
                }
                if (found_private_ipv4)
                {
                    addr = best_private_ipv4;
                    family = ip_address_family::ipv4;
                    return true;
                }
            }
#endif
            addr = {};
            addr[0] = 127;
            addr[3] = 1;
            family = ip_address_family::ipv4;
            return false;
        }
    } // anonymous namespace

    bool detect_routing_prefix(
        ip_address& addr,
        ip_address_family& family)
    {
        return detect_routing_prefix_impl(addr, family, nullptr);
    }

    bool detect_routing_prefix(
        ip_address& addr,
        ip_address_family& family,
        ip_address_family preferred_family)
    {
        return detect_routing_prefix_impl(addr, family, &preferred_family);
    }

    bool detect_host(
        ip_address& addr,
        ip_address_family& family)
    {
        return detect_host_impl(addr, family, nullptr);
    }

    bool detect_host(
        ip_address& addr,
        ip_address_family& family,
        ip_address_family preferred_family)
    {
        return detect_host_impl(addr, family, &preferred_family);
    }

    bool parse_ip_address(
        const std::string& str,
        ip_address& addr,
        ip_address_family family)
    {
#ifndef _WIN32
        addr = {};
        if (family == ip_address_family::ipv4)
        {
            struct in_addr a4;
            if (inet_pton(AF_INET, str.c_str(), &a4) == 1)
            {
                uint32_t h = ntohl(a4.s_addr);
                store_ipv4_bytes(h, addr);
                return true;
            }
        }
        else
        {
            struct in6_addr a6;
            if (inet_pton(AF_INET6, str.c_str(), &a6) == 1)
            {
                std::memcpy(addr.data(), a6.s6_addr, 16);
                return true;
            }
        }
#endif
        return false;
    }

} // namespace canopy::network_config
