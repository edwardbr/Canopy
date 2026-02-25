/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once
#include <string>
#include <stdint.h>
#include <functional>

#include "rpc/internal/coroutine_support.h"
#include <rpc/internal/serialiser.h>

namespace std
{
    inline std::string to_string(const rpc::zone_address& val)
    {
        if (val.routing_prefix == 0 && val.object_id == 0)
            return std::to_string(val.subnet_id);
        return std::to_string(val.routing_prefix) + ":" + std::to_string(val.subnet_id) + "/"
               + std::to_string(val.object_id);
    }
    inline std::string to_string(const rpc::zone& val)
    {
        return std::to_string(val.get_address());
    }
    inline std::string to_string(const rpc::destination_zone& val)
    {
        return std::to_string(val.get_address());
    }
    inline std::string to_string(const rpc::caller_zone& val)
    {
        return std::to_string(val.get_address());
    }
    inline std::string to_string(const rpc::known_direction_zone& val)
    {
        return std::to_string(val.get_address());
    }
    inline std::string to_string(const rpc::object& val)
    {
        return std::to_string(val.get_val());
    }
    inline std::string to_string(const rpc::interface_ordinal& val)
    {
        return std::to_string(val.get_val());
    }
    inline std::string to_string(const rpc::method& val)
    {
        return std::to_string(val.get_val());
    }

    template<> struct hash<rpc::zone_address>
    {
        auto operator()(const rpc::zone_address& item) const noexcept
        {
            // combine routing_prefix, subnet_id, and object_id
            std::size_t h = std::hash<uint64_t>{}(item.routing_prefix);
            h ^= std::hash<uint32_t>{}(item.subnet_id) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<uint32_t>{}(item.object_id) + 0x9e3779b9 + (h << 6) + (h >> 2);
            return h;
        }
    };

    template<> struct hash<rpc::zone>
    {
        auto operator()(const rpc::zone& item) const noexcept
        {
            return std::hash<rpc::zone_address>{}(item.get_address());
        }
    };

    template<> struct hash<rpc::destination_zone>
    {
        auto operator()(const rpc::destination_zone& item) const noexcept
        {
            return std::hash<rpc::zone_address>{}(item.get_address());
        }
    };

    template<> struct hash<rpc::caller_zone>
    {
        auto operator()(const rpc::caller_zone& item) const noexcept
        {
            return std::hash<rpc::zone_address>{}(item.get_address());
        }
    };

    template<> struct hash<rpc::known_direction_zone>
    {
        auto operator()(const rpc::known_direction_zone& item) const noexcept
        {
            return std::hash<rpc::zone_address>{}(item.get_address());
        }
    };

    template<> struct hash<rpc::interface_ordinal>
    {
        auto operator()(const rpc::interface_ordinal& item) const noexcept { return (std::size_t)item.get_val(); }
    };

    template<> struct hash<rpc::object>
    {
        auto operator()(const rpc::object& item) const noexcept { return (std::size_t)item.get_val(); }
    };

    template<> struct hash<rpc::method>
    {
        auto operator()(const rpc::method& item) const noexcept { return (std::size_t)item.get_val(); }
    };
}
